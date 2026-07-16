#include "capture/macos/MacDeviceCaptureBackend.h"

#include "capture/AudioCaptureBlockAssembler.h"
#include "capture/CameraCaptureFrameAssembler.h"
#include "capture/DeviceCaptureStopCoordinator.h"
#include "core/AppError.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace creator::capture::macos {
namespace {

std::string toUtf8(NSString* value) {
    if (value == nil) return {};
    const char* bytes = value.UTF8String;
    return bytes == nullptr ? std::string{} : std::string{bytes};
}

AVMediaType mediaType(CaptureDeviceKind kind) {
    return kind == CaptureDeviceKind::Camera ? AVMediaTypeVideo : AVMediaTypeAudio;
}

MediaPermissionStatus mapPermissionStatus(AVAuthorizationStatus status) noexcept {
    switch (status) {
    case AVAuthorizationStatusAuthorized:
        return MediaPermissionStatus::Granted;
    case AVAuthorizationStatusDenied:
        return MediaPermissionStatus::Denied;
    case AVAuthorizationStatusRestricted:
        return MediaPermissionStatus::Restricted;
    case AVAuthorizationStatusNotDetermined:
        return MediaPermissionStatus::Unknown;
    }
    return MediaPermissionStatus::Unknown;
}

NSArray<AVCaptureDeviceType>* cameraDeviceTypes() {
    NSMutableArray<AVCaptureDeviceType>* types =
        [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    }
    if (@available(macOS 13.0, *)) {
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    }
    return types;
}

AVCaptureDeviceDiscoverySession* discoverySession(CaptureDeviceKind kind) {
    NSArray<AVCaptureDeviceType>* types =
        kind == CaptureDeviceKind::Camera
            ? cameraDeviceTypes()
            : @[ AVCaptureDeviceTypeMicrophone ];
    return [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:types
                                mediaType:mediaType(kind)
                                 position:AVCaptureDevicePositionUnspecified];
}

core::AppError nativeFailure(const char* operation, NSError* error) {
    std::string message{operation};
    if (error != nil) {
        const std::string detail = toUtf8(error.localizedDescription);
        if (!detail.empty()) message += ": " + detail;
    }
    return {core::ErrorCode::IoFailure, std::move(message)};
}

bool numericTime(CMTime time) noexcept {
    return CMTIME_IS_VALID(time) && CMTIME_IS_NUMERIC(time) && time.timescale > 0;
}

class CameraDeliveryState final {
public:
    explicit CameraDeliveryState(std::shared_ptr<IVideoFrameSink> sink)
        : sink_(std::move(sink)) {}

    void started() noexcept {
        std::shared_ptr<IVideoFrameSink> sink;
        {
            std::scoped_lock lock{mutex_};
            if (!accepting_) return;
            sink = sink_;
        }
        if (sink) sink->onCaptureStarted();
    }

    void frame(CMSampleBufferRef sampleBuffer) noexcept {
        try {
            if (!CMSampleBufferDataIsReady(sampleBuffer)) {
                invalid();
                return;
            }
            CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
            const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
            if (image == nullptr || !numericTime(pts)) {
                invalid();
                return;
            }
            const auto width = CVPixelBufferGetWidth(image);
            const auto height = CVPixelBufferGetHeight(image);
            if (width == 0 || height == 0 ||
                width > std::numeric_limits<std::uint32_t>::max() ||
                height > std::numeric_limits<std::uint32_t>::max() ||
                CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_32BGRA) {
                invalid();
                return;
            }

            CVPixelBufferRetain(image);
            std::shared_ptr<void> handle{image, [](void* value) {
                                             CVPixelBufferRelease(
                                                 static_cast<CVPixelBufferRef>(value));
                                         }};
            auto assembled = assembler_.assemble(NativeCameraFrame{
                .timestamp = NativeTimestamp{pts.value, pts.timescale},
                .width = static_cast<std::uint32_t>(width),
                .height = static_cast<std::uint32_t>(height),
                .pixelFormat = media::PixelFormat::Bgra8,
                .platformHandle = std::move(handle),
            });
            if (!assembled.hasValue()) {
                invalid();
                return;
            }

            std::shared_ptr<IVideoFrameSink> sink;
            media::VideoFrame frame = std::move(assembled).value();
            {
                std::scoped_lock lock{mutex_};
                if (!accepting_) return;
                if (lastTimestamp_) {
                    const auto delta = frame.timestamp - *lastTimestamp_;
                    if (delta.count() > 0) {
                        stats_.currentFps = 1'000'000'000.0 /
                                            static_cast<double>(delta.count());
                    }
                }
                lastTimestamp_ = frame.timestamp;
                ++stats_.receivedFrames;
                sink = sink_;
            }
            if (sink) sink->onVideoFrame(std::move(frame));
        } catch (...) {
            terminal({core::ErrorCode::Unknown,
                      "Camera callback failed while importing a frame"});
        }
    }

    void dropped() noexcept {
        std::scoped_lock lock{mutex_};
        if (accepting_) ++stats_.droppedFrames;
    }

    void terminal(core::AppError error) noexcept {
        std::shared_ptr<IVideoFrameSink> sink;
        {
            std::scoped_lock lock{mutex_};
            if (!accepting_ || terminalSent_) return;
            terminalSent_ = true;
            accepting_ = false;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    void stopAccepting() noexcept {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
    }

    [[nodiscard]] CaptureStats stats() const noexcept {
        std::scoped_lock lock{mutex_};
        return stats_;
    }

private:
    void invalid() noexcept {
        std::scoped_lock lock{mutex_};
        if (accepting_) ++stats_.invalidFrames;
    }

    std::shared_ptr<IVideoFrameSink> sink_;
    CameraCaptureFrameAssembler assembler_;
    mutable std::mutex mutex_;
    CaptureStats stats_;
    std::optional<core::TimestampNs> lastTimestamp_;
    bool accepting_{true};
    bool terminalSent_{false};
};

class AudioDeliveryState final {
public:
    explicit AudioDeliveryState(std::shared_ptr<IAudioBlockSink> sink)
        : sink_(std::move(sink)) {}

    void started() noexcept {
        std::shared_ptr<IAudioBlockSink> sink;
        {
            std::scoped_lock lock{mutex_};
            if (!accepting_) return;
            sink = sink_;
        }
        if (sink) sink->onCaptureStarted();
    }

    void block(NativeAudioBlock native) noexcept {
        try {
            auto assembled = assembler_.assemble(std::move(native));
            if (!assembled.hasValue()) {
                terminal(assembled.error());
                return;
            }
            std::shared_ptr<IAudioBlockSink> sink;
            {
                std::scoped_lock lock{mutex_};
                if (!accepting_) return;
                sink = sink_;
            }
            if (sink) sink->onAudioBlock(std::move(assembled).value());
        } catch (...) {
            terminal({core::ErrorCode::Unknown,
                      "Audio callback failed while importing samples"});
        }
    }

    void terminal(core::AppError error) noexcept {
        std::shared_ptr<IAudioBlockSink> sink;
        {
            std::scoped_lock lock{mutex_};
            if (!accepting_ || terminalSent_) return;
            terminalSent_ = true;
            accepting_ = false;
            sink = sink_;
        }
        if (sink) sink->onCaptureError(std::move(error));
    }

    void stopAccepting() noexcept {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
    }

private:
    std::shared_ptr<IAudioBlockSink> sink_;
    AudioCaptureBlockAssembler assembler_;
    std::mutex mutex_;
    bool accepting_{true};
    bool terminalSent_{false};
};

core::Result<NativeAudioBlock> extractAudioBlock(CMSampleBufferRef sampleBuffer) {
    if (!CMSampleBufferDataIsReady(sampleBuffer)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio sample is not data-ready"};
    }
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    CMAudioFormatDescriptionRef description =
        CMSampleBufferGetFormatDescription(sampleBuffer);
    const AudioStreamBasicDescription* format = description == nullptr
                                                    ? nullptr
                                                    : CMAudioFormatDescriptionGetStreamBasicDescription(
                                                          description);
    const CMItemCount frameCount = CMSampleBufferGetNumSamples(sampleBuffer);
    if (!numericTime(pts) || format == nullptr || frameCount <= 0 ||
        frameCount > std::numeric_limits<std::uint32_t>::max() ||
        format->mFormatID != kAudioFormatLinearPCM ||
        (format->mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
        format->mBitsPerChannel != 32 || format->mChannelsPerFrame == 0 ||
        !std::isfinite(format->mSampleRate) || format->mSampleRate <= 0.0 ||
        format->mSampleRate > std::numeric_limits<std::uint32_t>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio sample has unsupported format metadata"};
    }

    const auto channels = static_cast<std::uint32_t>(format->mChannelsPerFrame);
    const auto frames = static_cast<std::uint32_t>(frameCount);
    const std::uint64_t sampleCount64 = static_cast<std::uint64_t>(channels) * frames;
    if (sampleCount64 > std::numeric_limits<std::size_t>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Native audio sample count exceeds memory limits"};
    }
    const auto sampleCount = static_cast<std::size_t>(sampleCount64);

    std::size_t listBytes = 0;
    OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, &listBytes, nullptr, 0, nullptr, nullptr, 0, nullptr);
    if (status != noErr || listBytes < sizeof(AudioBufferList)) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "CoreMedia could not size the native audio buffer list"};
    }
    const auto words = (listBytes + sizeof(std::max_align_t) - 1) /
                       sizeof(std::max_align_t);
    std::vector<std::max_align_t> storage(words);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    CMBlockBufferRef retainedBlock = nullptr;
    status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, nullptr, list, listBytes, kCFAllocatorDefault,
        kCFAllocatorDefault, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        &retainedBlock);
    if (status != noErr) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "CoreMedia could not read the native audio buffer list"};
    }
    auto retainedBlockGuard = std::unique_ptr<void, void (*)(void*)>{
        retainedBlock, [](void* value) {
            if (value != nullptr) CFRelease(value);
        }};
    auto samples = std::shared_ptr<float[]>(new float[sampleCount],
                                            std::default_delete<float[]>{});
    const bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (!planar) {
        if (list->mNumberBuffers < 1 || list->mBuffers[0].mData == nullptr ||
            list->mBuffers[0].mDataByteSize < sampleCount * sizeof(float)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Interleaved native audio buffer is truncated"};
        }
        std::memcpy(samples.get(), list->mBuffers[0].mData,
                    sampleCount * sizeof(float));
    } else {
        if (list->mNumberBuffers < channels) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Planar native audio buffer is missing channels"};
        }
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const AudioBuffer& buffer = list->mBuffers[channel];
            if (buffer.mData == nullptr ||
                buffer.mDataByteSize < static_cast<std::size_t>(frames) * sizeof(float)) {
                return core::AppError{core::ErrorCode::InvalidArgument,
                                      "Planar native audio buffer is truncated"};
            }
            const auto* source = static_cast<const float*>(buffer.mData);
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                samples[static_cast<std::size_t>(frame) * channels + channel] = source[frame];
            }
        }
    }
    return NativeAudioBlock{
        .timestamp = NativeTimestamp{pts.value, pts.timescale},
        .sampleRate = static_cast<std::uint32_t>(std::llround(format->mSampleRate)),
        .channels = channels,
        .frameCount = frames,
        .sampleCount = sampleCount,
        .samples = std::move(samples),
    };
}

@interface CSCameraCaptureBridge
    : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithState:(std::shared_ptr<CameraDeliveryState>)state;
@end

@implementation CSCameraCaptureBridge {
    std::shared_ptr<CameraDeliveryState> _state;
}
- (instancetype)initWithState:(std::shared_ptr<CameraDeliveryState>)state {
    self = [super init];
    if (self != nil) _state = std::move(state);
    return self;
}
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    static_cast<void>(output);
    static_cast<void>(connection);
    _state->frame(sampleBuffer);
}
- (void)captureOutput:(AVCaptureOutput*)output
    didDropSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    static_cast<void>(output);
    static_cast<void>(sampleBuffer);
    static_cast<void>(connection);
    _state->dropped();
}
@end

@interface CSAudioCaptureBridge
    : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
- (instancetype)initWithState:(std::shared_ptr<AudioDeliveryState>)state;
@end

@implementation CSAudioCaptureBridge {
    std::shared_ptr<AudioDeliveryState> _state;
}
- (instancetype)initWithState:(std::shared_ptr<AudioDeliveryState>)state {
    self = [super init];
    if (self != nil) _state = std::move(state);
    return self;
}
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    static_cast<void>(output);
    static_cast<void>(connection);
    try {
        auto block = extractAudioBlock(sampleBuffer);
        if (!block.hasValue()) {
            _state->terminal(block.error());
            return;
        }
        _state->block(std::move(block).value());
    } catch (...) {
        _state->terminal({core::ErrorCode::Unknown,
                          "Microphone callback failed while copying samples"});
    }
}
@end

class AvSessionRuntime final : public std::enable_shared_from_this<AvSessionRuntime> {
public:
    explicit AvSessionRuntime(dispatch_queue_t queue) : queue_(queue) {}

    void stop(IDeviceCaptureSource::StopCompletion completion) {
        coordinator_.add(std::move(completion));
        if (stopRequested_.exchange(true, std::memory_order_acq_rel)) return;
        if (cameraState_) cameraState_->stopAccepting();
        if (audioState_) audioState_->stopAccepting();
        auto retained = shared_from_this();
        dispatch_async(queue_, ^{
          if (retained->session_ != nil && retained->session_.running) {
              [retained->session_ stopRunning];
          }
          if ([retained->output_ isKindOfClass:AVCaptureVideoDataOutput.class]) {
              [(AVCaptureVideoDataOutput*)retained->output_ setSampleBufferDelegate:nil
                                                                              queue:nil];
          } else if ([retained->output_ isKindOfClass:AVCaptureAudioDataOutput.class]) {
              [(AVCaptureAudioDataOutput*)retained->output_ setSampleBufferDelegate:nil
                                                                              queue:nil];
          }
          retained->bridge_ = nil;
          retained->output_ = nil;
          retained->session_ = nil;
          retained->coordinator_.finish(core::ok());
        });
    }

    dispatch_queue_t __strong queue_{nil};
    AVCaptureSession* __strong session_{nil};
    AVCaptureOutput* __strong output_{nil};
    NSObject* __strong bridge_{nil};
    std::shared_ptr<CameraDeliveryState> cameraState_;
    std::shared_ptr<AudioDeliveryState> audioState_;
    std::atomic<bool> stopRequested_{false};
    DeviceCaptureStopCoordinator coordinator_;
};

class AvDeviceCaptureSource final : public IDeviceCaptureSource {
public:
    enum class Kind { Camera, Microphone };

    AvDeviceCaptureSource(domain::SourceId id, std::string name, AVCaptureDevice* device,
                          std::shared_ptr<IVideoFrameSink> sink)
        : id_(std::move(id)), name_(std::move(name)), device_(device), kind_(Kind::Camera),
          videoSink_(std::move(sink)) {}
    AvDeviceCaptureSource(domain::SourceId id, std::string name, AVCaptureDevice* device,
                          std::shared_ptr<IAudioBlockSink> sink)
        : id_(std::move(id)), name_(std::move(name)), device_(device),
          kind_(Kind::Microphone), audioSink_(std::move(sink)) {}

    ~AvDeviceCaptureSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return name_; }

    core::Result<void> start(const CaptureConfig& config) override {
        if (runtime_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "AVFoundation source is already started"};
        }
        if (device_ == nil || (kind_ == Kind::Camera && !videoSink_) ||
            (kind_ == Kind::Microphone && !audioSink_)) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "AVFoundation source is not fully configured"};
        }
        if (kind_ == Kind::Camera &&
            (config.targetWidth == 0 || config.targetHeight == 0 ||
             config.frameRateNumerator == 0 || config.frameRateDenominator == 0)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Camera capture configuration is invalid"};
        }

        dispatch_queue_t queue = dispatch_queue_create(
            kind_ == Kind::Camera ? "com.creatorstudio.camera-capture"
                                  : "com.creatorstudio.microphone-capture",
            DISPATCH_QUEUE_SERIAL);
        runtime_ = std::make_shared<AvSessionRuntime>(queue);
        if (kind_ == Kind::Camera) {
            runtime_->cameraState_ = std::make_shared<CameraDeliveryState>(videoSink_);
        } else {
            runtime_->audioState_ = std::make_shared<AudioDeliveryState>(audioSink_);
        }

        auto runtime = runtime_;
        AVCaptureDevice* retainedDevice = device_;
        const Kind kind = kind_;
        dispatch_async(queue, ^{
          if (runtime->stopRequested_.load(std::memory_order_acquire)) return;
          NSError* inputError = nil;
          AVCaptureDeviceInput* input =
              [AVCaptureDeviceInput deviceInputWithDevice:retainedDevice error:&inputError];
          if (input == nil) {
              const auto error = nativeFailure("Opening AVFoundation input", inputError);
              if (runtime->cameraState_) runtime->cameraState_->terminal(error);
              else runtime->audioState_->terminal(error);
              return;
          }

          AVCaptureSession* session = [AVCaptureSession new];
          [session beginConfiguration];
          if (![session canAddInput:input]) {
              [session commitConfiguration];
              const core::AppError error{core::ErrorCode::InvalidState,
                                         "AVFoundation rejected the capture device input"};
              if (runtime->cameraState_) runtime->cameraState_->terminal(error);
              else runtime->audioState_->terminal(error);
              return;
          }
          [session addInput:input];

          if (kind == Kind::Camera) {
              AVCaptureVideoDataOutput* output = [AVCaptureVideoDataOutput new];
              output.alwaysDiscardsLateVideoFrames = YES;
              output.videoSettings = @{
                  (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
              };
              CSCameraCaptureBridge* bridge =
                  [[CSCameraCaptureBridge alloc] initWithState:runtime->cameraState_];
              [output setSampleBufferDelegate:bridge queue:runtime->queue_];
              if (![session canAddOutput:output]) {
                  [session commitConfiguration];
                  runtime->cameraState_->terminal(
                      {core::ErrorCode::InvalidState,
                       "AVFoundation rejected the camera video output"});
                  return;
              }
              [session addOutput:output];
              if ([session canSetSessionPreset:AVCaptureSessionPreset1920x1080]) {
                  session.sessionPreset = AVCaptureSessionPreset1920x1080;
              } else if ([session canSetSessionPreset:AVCaptureSessionPresetHigh]) {
                  session.sessionPreset = AVCaptureSessionPresetHigh;
              }
              runtime->bridge_ = bridge;
              runtime->output_ = output;
          } else {
              AVCaptureAudioDataOutput* output = [AVCaptureAudioDataOutput new];
              output.audioSettings = @{
                  AVFormatIDKey : @(kAudioFormatLinearPCM),
                  AVLinearPCMIsFloatKey : @YES,
                  AVLinearPCMBitDepthKey : @32,
                  AVLinearPCMIsNonInterleaved : @NO,
                  AVSampleRateKey : @48000
              };
              CSAudioCaptureBridge* bridge =
                  [[CSAudioCaptureBridge alloc] initWithState:runtime->audioState_];
              [output setSampleBufferDelegate:bridge queue:runtime->queue_];
              if (![session canAddOutput:output]) {
                  [session commitConfiguration];
                  runtime->audioState_->terminal(
                      {core::ErrorCode::InvalidState,
                       "AVFoundation rejected the microphone audio output"});
                  return;
              }
              [session addOutput:output];
              runtime->bridge_ = bridge;
              runtime->output_ = output;
          }
          [session commitConfiguration];
          runtime->session_ = session;
          if (runtime->stopRequested_.load(std::memory_order_acquire)) return;
          [session startRunning];
          if (!session.running) {
              const core::AppError error{core::ErrorCode::IoFailure,
                                         "AVFoundation session did not start running"};
              if (runtime->cameraState_) runtime->cameraState_->terminal(error);
              else runtime->audioState_->terminal(error);
          } else if (runtime->cameraState_) {
              runtime->cameraState_->started();
          } else {
              runtime->audioState_->started();
          }
        });
        return core::ok();
    }

    core::Result<void> stop() override {
        if (runtime_) runtime_->stop({});
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (!runtime_) {
            if (completion) completion(core::ok());
            return;
        }
        runtime_->stop(std::move(completion));
    }

    [[nodiscard]] CaptureStats stats() const noexcept override {
        return runtime_ && runtime_->cameraState_ ? runtime_->cameraState_->stats()
                                                  : CaptureStats{};
    }

private:
    domain::SourceId id_;
    std::string name_;
    AVCaptureDevice* __strong device_{nil};
    Kind kind_;
    std::shared_ptr<IVideoFrameSink> videoSink_;
    std::shared_ptr<IAudioBlockSink> audioSink_;
    std::shared_ptr<AvSessionRuntime> runtime_;
};

@interface CSSystemAudioBridge : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithState:(std::shared_ptr<AudioDeliveryState>)state;
@end

@implementation CSSystemAudioBridge {
    std::shared_ptr<AudioDeliveryState> _state;
}
- (instancetype)initWithState:(std::shared_ptr<AudioDeliveryState>)state {
    self = [super init];
    if (self != nil) _state = std::move(state);
    return self;
}
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                  ofType:(SCStreamOutputType)type {
    static_cast<void>(stream);
    if (type != SCStreamOutputTypeAudio) return;
    try {
        auto block = extractAudioBlock(sampleBuffer);
        if (!block.hasValue()) {
            _state->terminal(block.error());
            return;
        }
        _state->block(std::move(block).value());
    } catch (...) {
        _state->terminal({core::ErrorCode::Unknown,
                          "System audio callback failed while copying samples"});
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    static_cast<void>(stream);
    _state->terminal(nativeFailure("ScreenCaptureKit system audio stopped", error));
}
@end

class SystemAudioRuntime final
    : public std::enable_shared_from_this<SystemAudioRuntime> {
public:
    explicit SystemAudioRuntime(std::shared_ptr<AudioDeliveryState> state)
        : state_(std::move(state)),
          queue_(dispatch_queue_create("com.creatorstudio.system-audio-capture",
                                       DISPATCH_QUEUE_SERIAL)) {}

    void start() {
        auto retained = shared_from_this();
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:YES
                                     onScreenWindowsOnly:NO
                                      completionHandler:^(SCShareableContent* content,
                                                          NSError* error) {
          dispatch_async(retained->queue_, ^{
            if (retained->stopRequested_.load(std::memory_order_acquire)) return;
            if (error != nil || content.displays.count == 0) {
                retained->state_->terminal(
                    error != nil
                        ? nativeFailure("Discovering a display for system audio", error)
                        : core::AppError{core::ErrorCode::NotFound,
                                         "No display is available for system audio capture"});
                return;
            }

            SCDisplay* display = content.displays.firstObject;
            SCContentFilter* filter =
                [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
            SCStreamConfiguration* configuration = [SCStreamConfiguration new];
            configuration.capturesAudio = YES;
            configuration.excludesCurrentProcessAudio = YES;
            configuration.sampleRate = 48'000;
            configuration.channelCount = 2;
            configuration.width = 2;
            configuration.height = 2;
            configuration.queueDepth = 3;
            configuration.showsCursor = NO;

            CSSystemAudioBridge* bridge =
                [[CSSystemAudioBridge alloc] initWithState:retained->state_];
            SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                                  configuration:configuration
                                                       delegate:bridge];
            NSError* outputError = nil;
            if (![stream addStreamOutput:bridge
                                    type:SCStreamOutputTypeAudio
                      sampleHandlerQueue:retained->queue_
                                   error:&outputError]) {
                retained->state_->terminal(nativeFailure(
                    "Adding ScreenCaptureKit system audio output", outputError));
                return;
            }
            retained->filter_ = filter;
            retained->bridge_ = bridge;
            retained->stream_ = stream;
            [stream startCaptureWithCompletionHandler:^(NSError* startError) {
              if (startError != nil) {
                  retained->state_->terminal(
                      nativeFailure("Starting ScreenCaptureKit system audio", startError));
              } else {
                  retained->state_->started();
              }
            }];
          });
        }];
    }

    void stop(IDeviceCaptureSource::StopCompletion completion) {
        coordinator_.add(std::move(completion));
        if (stopRequested_.exchange(true, std::memory_order_acq_rel)) return;
        state_->stopAccepting();
        auto retained = shared_from_this();
        dispatch_async(queue_, ^{
          if (retained->stream_ == nil) {
              retained->coordinator_.finish(core::ok());
              return;
          }
          SCStream* stream = retained->stream_;
          CSSystemAudioBridge* bridge = retained->bridge_;
          [stream stopCaptureWithCompletionHandler:^(NSError* stopError) {
            dispatch_async(retained->queue_, ^{
              NSError* removeError = nil;
              const BOOL removed = [stream removeStreamOutput:bridge
                                                         type:SCStreamOutputTypeAudio
                                                        error:&removeError];
              retained->stream_ = nil;
              retained->bridge_ = nil;
              retained->filter_ = nil;
              if (stopError != nil) {
                  retained->coordinator_.finish(nativeFailure(
                      "Stopping ScreenCaptureKit system audio", stopError));
              } else if (!removed) {
                  retained->coordinator_.finish(nativeFailure(
                      "Removing ScreenCaptureKit system audio output", removeError));
              } else {
                  retained->coordinator_.finish(core::ok());
              }
            });
          }];
        });
    }

private:
    std::shared_ptr<AudioDeliveryState> state_;
    dispatch_queue_t __strong queue_{nil};
    SCContentFilter* __strong filter_{nil};
    SCStream* __strong stream_{nil};
    CSSystemAudioBridge* __strong bridge_{nil};
    std::atomic<bool> stopRequested_{false};
    DeviceCaptureStopCoordinator coordinator_;
};

class SystemAudioCaptureSource final : public IDeviceCaptureSource {
public:
    explicit SystemAudioCaptureSource(std::shared_ptr<IAudioBlockSink> sink)
        : id_(domain::SourceId::create("system-audio").value()),
          sink_(std::move(sink)) {}

    ~SystemAudioCaptureSource() override { static_cast<void>(stop()); }
    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return "System Audio"; }

    core::Result<void> start(const CaptureConfig&) override {
        if (runtime_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "System audio source is already started"};
        }
        if (!sink_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "System audio source has no sink"};
        }
        if (!CGPreflightScreenCaptureAccess()) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                "Screen recording permission is required for system audio capture"};
        }
        auto state = std::make_shared<AudioDeliveryState>(sink_);
        runtime_ = std::make_shared<SystemAudioRuntime>(std::move(state));
        runtime_->start();
        return core::ok();
    }

    core::Result<void> stop() override {
        if (runtime_) runtime_->stop({});
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (!runtime_) {
            if (completion) completion(core::ok());
            return;
        }
        runtime_->stop(std::move(completion));
    }

    [[nodiscard]] CaptureStats stats() const noexcept override { return {}; }

private:
    domain::SourceId id_;
    std::shared_ptr<IAudioBlockSink> sink_;
    std::shared_ptr<SystemAudioRuntime> runtime_;
};

class HotplugState final {
public:
    void set(IDeviceCaptureBackend::DeviceChangeHandler handler) {
        std::scoped_lock lock{mutex_};
        handler_ = std::move(handler);
    }

    void fire() noexcept {
        try {
            IDeviceCaptureBackend::DeviceChangeHandler handler;
            {
                std::scoped_lock lock{mutex_};
                handler = handler_;
            }
            if (handler) handler();
        } catch (...) {
            // A C++ exception must never unwind through NotificationCenter.
        }
    }

private:
    std::mutex mutex_;
    IDeviceCaptureBackend::DeviceChangeHandler handler_;
};

class MacDeviceCaptureBackend final : public IDeviceCaptureBackend {
public:
    MacDeviceCaptureBackend() : hotplug_(std::make_shared<HotplugState>()) {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto state = hotplug_;
        connectedObserver_ = [center
            addObserverForName:AVCaptureDeviceWasConnectedNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) { state->fire(); }];
        disconnectedObserver_ = [center
            addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) { state->fire(); }];
    }

    ~MacDeviceCaptureBackend() override {
        hotplug_->set({});
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (connectedObserver_ != nil) [center removeObserver:connectedObserver_];
        if (disconnectedObserver_ != nil) [center removeObserver:disconnectedObserver_];
    }

    [[nodiscard]] MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept override {
        return mapPermissionStatus(
            [AVCaptureDevice authorizationStatusForMediaType:mediaType(kind)]);
    }

    void requestPermission(CaptureDeviceKind kind,
                           PermissionCompletion completion) override {
        auto retained = std::make_shared<PermissionCompletion>(std::move(completion));
        [AVCaptureDevice requestAccessForMediaType:mediaType(kind)
                                completionHandler:^(BOOL granted) {
          if (!*retained) return;
          const auto status = granted ? MediaPermissionStatus::Granted
                                      : mapPermissionStatus(
                                            [AVCaptureDevice
                                                authorizationStatusForMediaType:mediaType(kind)]);
          (*retained)(status);
        }];
    }

    [[nodiscard]] core::Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) override {
        AVCaptureDeviceDiscoverySession* session = discoverySession(kind);
        if (session == nil) {
            return core::AppError{core::ErrorCode::Unknown,
                                  "AVFoundation device discovery could not be created"};
        }
        AVCaptureDevice* defaultDevice =
            [AVCaptureDevice defaultDeviceWithMediaType:mediaType(kind)];
        NSString* defaultId = defaultDevice.uniqueID;
        std::vector<CaptureDeviceInfo> result;
        result.reserve(static_cast<std::size_t>(session.devices.count));
        for (AVCaptureDevice* device in session.devices) {
            const std::string idValue = toUtf8(device.uniqueID);
            const std::string name = toUtf8(device.localizedName);
            if (idValue.empty() || name.empty() || !device.connected) continue;
            auto id = domain::CaptureDeviceId::create(idValue);
            if (!id.hasValue()) continue;
            auto info = CaptureDeviceInfo::create(
                std::move(id).value(), kind, name,
                defaultId != nil && [device.uniqueID isEqualToString:defaultId]);
            if (info.hasValue()) result.push_back(std::move(info).value());
        }
        return result;
    }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        hotplug_->set(std::move(handler));
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createCamera(
        const domain::CaptureDeviceId& deviceId,
        std::shared_ptr<IVideoFrameSink> sink) override {
        NSString* nativeId = [NSString stringWithUTF8String:deviceId.value().c_str()];
        AVCaptureDevice* device = nativeId == nil
                                      ? nil
                                      : [AVCaptureDevice deviceWithUniqueID:nativeId];
        if (device == nil || ![device hasMediaType:AVMediaTypeVideo]) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "Selected camera is no longer available"};
        }
        auto sourceId = domain::SourceId::create("camera:" + deviceId.value());
        if (!sourceId.hasValue()) return sourceId.error();
        std::unique_ptr<IDeviceCaptureSource> source =
            std::make_unique<AvDeviceCaptureSource>(
                std::move(sourceId).value(), toUtf8(device.localizedName), device,
                std::move(sink));
        return source;
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createMicrophone(
        const domain::CaptureDeviceId& deviceId,
        std::shared_ptr<IAudioBlockSink> sink) override {
        NSString* nativeId = [NSString stringWithUTF8String:deviceId.value().c_str()];
        AVCaptureDevice* device = nativeId == nil
                                      ? nil
                                      : [AVCaptureDevice deviceWithUniqueID:nativeId];
        if (device == nil || ![device hasMediaType:AVMediaTypeAudio]) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "Selected microphone is no longer available"};
        }
        auto sourceId = domain::SourceId::create("microphone:" + deviceId.value());
        if (!sourceId.hasValue()) return sourceId.error();
        std::unique_ptr<IDeviceCaptureSource> source =
            std::make_unique<AvDeviceCaptureSource>(
                std::move(sourceId).value(), toUtf8(device.localizedName), device,
                std::move(sink));
        return source;
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createSystemAudio(
        std::shared_ptr<IAudioBlockSink> sink) override {
        std::unique_ptr<IDeviceCaptureSource> source =
            std::make_unique<SystemAudioCaptureSource>(std::move(sink));
        return source;
    }

private:
    std::shared_ptr<HotplugState> hotplug_;
    id __strong connectedObserver_{nil};
    id __strong disconnectedObserver_{nil};
};

}  // namespace

std::unique_ptr<IDeviceCaptureBackend> makeMacDeviceCaptureBackend() {
    return std::make_unique<MacDeviceCaptureBackend>();
}

}  // namespace creator::capture::macos
