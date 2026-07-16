#include "capture/macos/MacScreenCaptureBackend.h"

#include "capture/NumericCaptureTargetId.h"
#include "capture/ScreenCaptureFrameAssembler.h"
#include "core/AppError.h"
#include "core/Timebase.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

typedef void (*CSMacFrameCallback)(void* context, CMSampleBufferRef sampleBuffer);
typedef void (*CSMacErrorCallback)(void* context, NSError* error);
typedef void (*CSMacDestroyCallback)(void* context);

@interface CSMacCaptureTargetRegistry : NSObject
@property(nonatomic, copy) NSDictionary<NSString*, SCDisplay*>* displays;
@property(nonatomic, copy) NSDictionary<NSString*, SCWindow*>* windows;
@property(nonatomic, copy) NSArray<SCWindow*>* ownWindows;
@end

@implementation CSMacCaptureTargetRegistry
@end

@interface CSMacStreamBridge : NSObject <SCStreamDelegate, SCStreamOutput> {
@private
    void* _context;
    CSMacFrameCallback _frameCallback;
    CSMacErrorCallback _errorCallback;
    CSMacDestroyCallback _destroyCallback;
}
- (instancetype)initWithContext:(void*)context
                  frameCallback:(CSMacFrameCallback)frameCallback
                  errorCallback:(CSMacErrorCallback)errorCallback
                destroyCallback:(CSMacDestroyCallback)destroyCallback;
@end

namespace creator::capture::macos {
namespace {

std::string toUtf8(NSString* value) {
    if (value == nil) return {};
    const char* bytes = value.UTF8String;
    return bytes == nullptr ? std::string{} : std::string{bytes};
}

core::AppError nativeFailure(const char* operation, NSError* error) {
    std::string message{operation};
    message += " failed";
    if (error != nil && error.localizedDescription.length > 0) {
        message += ": ";
        message += toUtf8(error.localizedDescription);
    }
    return core::AppError{core::ErrorCode::IoFailure, std::move(message)};
}

std::uint32_t dimension(CGFloat value) {
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<CGFloat>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::ceil(value));
}

NativeScreenFrameStatus frameStatus(SCFrameStatus status) {
    switch (status) {
        case SCFrameStatusComplete:  return NativeScreenFrameStatus::Complete;
        case SCFrameStatusIdle:      return NativeScreenFrameStatus::Idle;
        case SCFrameStatusBlank:     return NativeScreenFrameStatus::Blank;
        case SCFrameStatusStarted:   return NativeScreenFrameStatus::Started;
        case SCFrameStatusSuspended: return NativeScreenFrameStatus::Suspended;
        case SCFrameStatusStopped:   return NativeScreenFrameStatus::Stopped;
    }
    return NativeScreenFrameStatus::Blank;
}

class MacStreamState final {
public:
    explicit MacStreamState(std::shared_ptr<IVideoFrameSink> sink)
        : sink_(std::move(sink)), assembler_(core::ProjectClock::now()) {}

    void notifyStarted() noexcept {
        std::scoped_lock lock{mutex_};
        if (!accepting_ || started_ || !sink_) return;
        started_ = true;
        sink_->onCaptureStarted();
    }

    void deliver(NativeScreenFrame frame) noexcept {
        std::scoped_lock lock{mutex_};
        if (!accepting_ || !sink_) return;
        try {
            auto assembled = assembler_.assemble(std::move(frame));
            if (!assembled.hasValue()) {
                ++stats_.droppedFrames;
                accepting_ = false;
                sink_->onCaptureError(assembled.error());
                sink_.reset();
                return;
            }
            if (!assembled.value()) {
                ++stats_.droppedFrames;
                return;
            }

            auto videoFrame = std::move(*assembled.value());
            if (!firstTimestamp_) firstTimestamp_ = videoFrame.timestamp;
            lastTimestamp_ = videoFrame.timestamp;
            ++stats_.receivedFrames;
            if (stats_.receivedFrames > 1 && firstTimestamp_ && lastTimestamp_) {
                const auto elapsed = (*lastTimestamp_ - *firstTimestamp_).count();
                if (elapsed > 0) {
                    stats_.currentFps =
                        static_cast<double>(stats_.receivedFrames - 1) * 1'000'000'000.0 /
                        static_cast<double>(elapsed);
                }
            }
            sink_->onVideoFrame(std::move(videoFrame));
        } catch (...) {
            ++stats_.droppedFrames;
            accepting_ = false;
            sink_->onCaptureError(core::AppError{
                core::ErrorCode::Unknown, "ScreenCaptureKit frame processing failed"});
            sink_.reset();
        }
    }

    void dropMalformedFrame() noexcept {
        std::scoped_lock lock{mutex_};
        if (accepting_) ++stats_.droppedFrames;
    }

    void terminal(NSError* error) noexcept {
        std::scoped_lock lock{mutex_};
        if (!accepting_ || !sink_) return;
        accepting_ = false;
        sink_->onCaptureError(nativeFailure("ScreenCaptureKit stream", error));
        sink_.reset();
    }

    void stopAccepting() noexcept {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
        sink_.reset();
    }

    [[nodiscard]] CaptureStats stats() const noexcept {
        std::scoped_lock lock{mutex_};
        return stats_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<IVideoFrameSink> sink_;
    ScreenCaptureFrameAssembler assembler_;
    CaptureStats stats_;
    std::optional<core::TimestampNs> firstTimestamp_;
    std::optional<core::TimestampNs> lastTimestamp_;
    bool accepting_{true};
    bool started_{false};
};

using RetainedStreamState = std::shared_ptr<MacStreamState>;

void releaseStreamContext(void* context) noexcept {
    delete static_cast<RetainedStreamState*>(context);
}

void receiveFrame(void* context, CMSampleBufferRef sampleBuffer) {
    auto state = *static_cast<RetainedStreamState*>(context);
    if (sampleBuffer == nullptr || !CMSampleBufferIsValid(sampleBuffer) ||
        !CMSampleBufferDataIsReady(sampleBuffer)) {
        state->dropMalformedFrame();
        return;
    }

    CFArrayRef attachments =
        CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments == nullptr || CFArrayGetCount(attachments) == 0) {
        state->dropMalformedFrame();
        return;
    }
    NSDictionary* metadata =
        (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* statusNumber = metadata[SCStreamFrameInfoStatus];
    if (statusNumber == nil) {
        state->dropMalformedFrame();
        return;
    }

    const auto status = frameStatus(static_cast<SCFrameStatus>(statusNumber.integerValue));
    if (status != NativeScreenFrameStatus::Complete) {
        state->deliver(NativeScreenFrame{.status = status});
        return;
    }

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    const CMTime presentationTime = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    if (pixelBuffer == nullptr || !CMTIME_IS_VALID(presentationTime) ||
        presentationTime.timescale <= 0) {
        state->dropMalformedFrame();
        return;
    }

    const auto nativeWidth = CVPixelBufferGetWidth(pixelBuffer);
    const auto nativeHeight = CVPixelBufferGetHeight(pixelBuffer);
    if (nativeWidth == 0 || nativeHeight == 0 ||
        nativeWidth > std::numeric_limits<std::uint32_t>::max() ||
        nativeHeight > std::numeric_limits<std::uint32_t>::max()) {
        state->dropMalformedFrame();
        return;
    }

    const auto nativeFormat = CVPixelBufferGetPixelFormatType(pixelBuffer);
    const auto pixelFormat = nativeFormat == kCVPixelFormatType_32BGRA
                                 ? media::PixelFormat::Bgra8
                                 : media::PixelFormat::Unknown;
    CVPixelBufferRetain(pixelBuffer);
    std::shared_ptr<void> retainedBuffer{
        pixelBuffer,
        [](void* buffer) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(buffer)); }};
    state->deliver(NativeScreenFrame{
        .status = NativeScreenFrameStatus::Complete,
        .timestamp = NativeTimestamp{.value = presentationTime.value,
                                     .timescale = presentationTime.timescale},
        .width = static_cast<std::uint32_t>(nativeWidth),
        .height = static_cast<std::uint32_t>(nativeHeight),
        .pixelFormat = pixelFormat,
        .platformHandle = std::move(retainedBuffer),
    });
}

void receiveError(void* context, NSError* error) {
    auto state = *static_cast<RetainedStreamState*>(context);
    state->terminal(error);
}

class MacScreenCapturePermission final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override {
        return CGPreflightScreenCaptureAccess() ? ScreenCapturePermissionStatus::Granted
                                                : ScreenCapturePermissionStatus::Denied;
    }

    void request(Completion completion) override {
        auto retainedCompletion = std::make_shared<Completion>(std::move(completion));
        dispatch_async(dispatch_get_main_queue(), ^{
          const auto result = CGRequestScreenCaptureAccess()
                                  ? ScreenCapturePermissionStatus::Granted
                                  : ScreenCapturePermissionStatus::Denied;
          (*retainedCompletion)(result);
        });
    }
};

class MacScreenCaptureDiscovery final : public IScreenCaptureDiscovery {
public:
    explicit MacScreenCaptureDiscovery(CSMacCaptureTargetRegistry* registry)
        : registry_(registry) {}

    void enumerate(Completion completion) override {
        auto retainedCompletion = std::make_shared<Completion>(std::move(completion));
        CSMacCaptureTargetRegistry* retainedRegistry = registry_;
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                   completionHandler:^(SCShareableContent* content,
                                                       NSError* error) {
          if (error != nil || content == nil) {
              (*retainedCompletion)(nativeFailure("ScreenCaptureKit discovery", error));
              return;
          }

          NSMutableDictionary<NSString*, SCDisplay*>* displays =
              [NSMutableDictionary dictionary];
          NSMutableDictionary<NSString*, SCWindow*>* windows =
              [NSMutableDictionary dictionary];
          NSMutableArray<SCWindow*>* ownWindows = [NSMutableArray array];
          std::vector<ScreenCaptureTarget> targets;
          targets.reserve(content.displays.count + content.windows.count);

          for (SCDisplay* display in content.displays) {
              auto id = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Display,
                                                   display.displayID);
              if (!id.hasValue()) continue;
              const auto width = dimension(static_cast<CGFloat>(display.width));
              const auto height = dimension(static_cast<CGFloat>(display.height));
              if (width == 0 || height == 0) continue;
              const std::string name =
                  "Display " + std::to_string(static_cast<std::uint64_t>(display.displayID));
              auto target = ScreenCaptureTarget::create(id.value(),
                                                        ScreenCaptureTargetKind::Display,
                                                        name, std::nullopt, width, height);
              if (!target.hasValue()) continue;
              NSString* key = [NSString stringWithUTF8String:id.value().value().c_str()];
              if (key == nil) continue;
              displays[key] = display;
              targets.push_back(std::move(target).value());
          }

          const pid_t ownProcess = NSProcessInfo.processInfo.processIdentifier;
          for (SCWindow* window in content.windows) {
              if (window.owningApplication.processID == ownProcess) {
                  [ownWindows addObject:window];
                  continue;
              }
              auto id = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Window,
                                                   window.windowID);
              if (!id.hasValue()) continue;
              const auto width = dimension(window.frame.size.width);
              const auto height = dimension(window.frame.size.height);
              if (width == 0 || height == 0) continue;

              std::string title = toUtf8(window.title);
              if (title.empty()) {
                  title = "Window " +
                          std::to_string(static_cast<std::uint64_t>(window.windowID));
              }
              std::optional<std::string> applicationName;
              const std::string appName = toUtf8(window.owningApplication.applicationName);
              if (!appName.empty()) applicationName = appName;
              auto target = ScreenCaptureTarget::create(
                  id.value(), ScreenCaptureTargetKind::Window, std::move(title),
                  std::move(applicationName), width, height);
              if (!target.hasValue()) continue;
              NSString* key = [NSString stringWithUTF8String:id.value().value().c_str()];
              if (key == nil) continue;
              windows[key] = window;
              targets.push_back(std::move(target).value());
          }

          @synchronized(retainedRegistry) {
              retainedRegistry.displays = displays;
              retainedRegistry.windows = windows;
              retainedRegistry.ownWindows = ownWindows;
          }
          (*retainedCompletion)(std::move(targets));
        }];
    }

private:
    CSMacCaptureTargetRegistry* __strong registry_;
};

class MacScreenCaptureSource final : public ICaptureSource {
public:
    MacScreenCaptureSource(domain::SourceId id, std::string displayName,
                           SCContentFilter* filter, std::shared_ptr<IVideoFrameSink> sink)
        : id_(std::move(id)),
          displayName_(std::move(displayName)),
          filter_(filter),
          sink_(std::move(sink)) {}

    ~MacScreenCaptureSource() override { static_cast<void>(stop()); }

    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return displayName_; }

    core::Result<void> start(const CaptureConfig& config) override {
        if (started_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "ScreenCaptureKit source is already started"};
        }
        if (!sink_ || filter_ == nil) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "ScreenCaptureKit source is not fully configured"};
        }
        if (config.targetWidth == 0 || config.targetHeight == 0 ||
            config.frameRateNumerator == 0 || config.frameRateDenominator == 0 ||
            config.frameRateNumerator >
                static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
            config.targetWidth >
                static_cast<std::uint32_t>(std::numeric_limits<NSInteger>::max()) ||
            config.targetHeight >
                static_cast<std::uint32_t>(std::numeric_limits<NSInteger>::max())) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "ScreenCaptureKit capture configuration is invalid"};
        }

        SCStreamConfiguration* configuration = [SCStreamConfiguration new];
        configuration.width = static_cast<NSInteger>(config.targetWidth);
        configuration.height = static_cast<NSInteger>(config.targetHeight);
        configuration.minimumFrameInterval =
            CMTimeMake(config.frameRateDenominator,
                       static_cast<std::int32_t>(config.frameRateNumerator));
        configuration.queueDepth = 3;
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.showsCursor = YES;
        configuration.capturesAudio = NO;

        state_ = std::make_shared<MacStreamState>(sink_);
        auto* context = new RetainedStreamState{state_};
        bridge_ = [[CSMacStreamBridge alloc] initWithContext:context
                                              frameCallback:&receiveFrame
                                              errorCallback:&receiveError
                                            destroyCallback:&releaseStreamContext];
        if (bridge_ == nil) {
            releaseStreamContext(context);
            state_.reset();
            return core::AppError{core::ErrorCode::Unknown,
                                  "ScreenCaptureKit callback bridge could not be created"};
        }

        outputQueue_ = dispatch_queue_create("com.creatorstudio.screen-capture",
                                             DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter_
                                    configuration:configuration
                                         delegate:bridge_];
        NSError* outputError = nil;
        if (![stream_ addStreamOutput:bridge_
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:outputQueue_
                                error:&outputError]) {
            state_->stopAccepting();
            lastStats_ = state_->stats();
            state_.reset();
            stream_ = nil;
            bridge_ = nil;
            outputQueue_ = nil;
            return nativeFailure("Adding ScreenCaptureKit video output", outputError);
        }

        started_ = true;
        auto retainedState = state_;
        [stream_ startCaptureWithCompletionHandler:^(NSError* error) {
          if (error != nil) {
              retainedState->terminal(error);
          } else {
              retainedState->notifyStarted();
          }
        }];
        return core::ok();
    }

    core::Result<void> stop() override {
        if (!started_) return core::ok();
        started_ = false;
        state_->stopAccepting();
        lastStats_ = state_->stats();

        NSError* outputError = nil;
        const BOOL removed = [stream_ removeStreamOutput:bridge_
                                                  type:SCStreamOutputTypeScreen
                                                 error:&outputError];
        [stream_ stopCaptureWithCompletionHandler:nil];
        state_.reset();
        stream_ = nil;
        bridge_ = nil;
        outputQueue_ = nil;
        if (!removed) {
            return nativeFailure("Removing ScreenCaptureKit video output", outputError);
        }
        return core::ok();
    }

    [[nodiscard]] CaptureStats stats() const noexcept override {
        return state_ ? state_->stats() : lastStats_;
    }

private:
    domain::SourceId id_;
    std::string displayName_;
    SCContentFilter* __strong filter_;
    std::shared_ptr<IVideoFrameSink> sink_;
    std::shared_ptr<MacStreamState> state_;
    SCStream* __strong stream_{nil};
    CSMacStreamBridge* __strong bridge_{nil};
    dispatch_queue_t __strong outputQueue_{nil};
    CaptureStats lastStats_;
    bool started_{false};
};

class MacScreenCaptureSourceFactory final : public IScreenCaptureSourceFactory {
public:
    explicit MacScreenCaptureSourceFactory(CSMacCaptureTargetRegistry* registry)
        : registry_(registry) {}

    core::Result<std::unique_ptr<ICaptureSource>> create(
        const domain::CaptureTargetId& targetId,
        std::shared_ptr<IVideoFrameSink> sink) override {
        NSString* key = [NSString stringWithUTF8String:targetId.value().c_str()];
        if (key == nil) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "capture target id is not valid UTF-8"};
        }

        SCDisplay* display = nil;
        SCWindow* window = nil;
        NSArray<SCWindow*>* ownWindows = nil;
        @synchronized(registry_) {
            display = registry_.displays[key];
            window = registry_.windows[key];
            ownWindows = registry_.ownWindows;
        }
        if (display == nil && window == nil) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "selected screen capture target is no longer available"};
        }

        NSArray<SCWindow*>* excludedWindows = ownWindows != nil ? ownWindows : @[];
        SCContentFilter* filter = display != nil
                                      ? [[SCContentFilter alloc]
                                            initWithDisplay:display
                                           excludingWindows:excludedWindows]
                                      : [[SCContentFilter alloc]
                                            initWithDesktopIndependentWindow:window];
        auto sourceId = domain::SourceId::create("screen-preview:" + targetId.value());
        if (!sourceId.hasValue()) return sourceId.error();
        return std::unique_ptr<ICaptureSource>{std::make_unique<MacScreenCaptureSource>(
            std::move(sourceId).value(), targetId.value(), filter, std::move(sink))};
    }

private:
    CSMacCaptureTargetRegistry* __strong registry_;
};

}  // namespace

MacScreenCaptureBackend makeMacScreenCaptureBackend() {
    CSMacCaptureTargetRegistry* registry = [CSMacCaptureTargetRegistry new];
    registry.displays = @{};
    registry.windows = @{};
    registry.ownWindows = @[];
    return MacScreenCaptureBackend{
        .permission = std::make_unique<MacScreenCapturePermission>(),
        .discovery = std::make_unique<MacScreenCaptureDiscovery>(registry),
        .sourceFactory = std::make_unique<MacScreenCaptureSourceFactory>(registry),
    };
}

}  // namespace creator::capture::macos

@implementation CSMacStreamBridge

- (instancetype)initWithContext:(void*)context
                  frameCallback:(CSMacFrameCallback)frameCallback
                  errorCallback:(CSMacErrorCallback)errorCallback
                destroyCallback:(CSMacDestroyCallback)destroyCallback {
    self = [super init];
    if (self != nil) {
        _context = context;
        _frameCallback = frameCallback;
        _errorCallback = errorCallback;
        _destroyCallback = destroyCallback;
    }
    return self;
}

- (void)dealloc {
    if (_destroyCallback != nullptr) _destroyCallback(_context);
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    static_cast<void>(stream);
    if (type == SCStreamOutputTypeScreen && _frameCallback != nullptr) {
        @autoreleasepool {
            _frameCallback(_context, sampleBuffer);
        }
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    static_cast<void>(stream);
    if (_errorCallback != nullptr) _errorCallback(_context, error);
}

@end
