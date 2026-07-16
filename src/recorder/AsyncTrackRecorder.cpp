#include "recorder/AsyncTrackRecorder.h"

#include "core/AppError.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace creator::recorder {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

core::AppError invalidState(std::string message) {
    return {core::ErrorCode::InvalidState, std::move(message)};
}

core::TimestampNs audioTimestamp(const media::AudioBlock& block,
                                 std::uint64_t frameOffset) {
    const auto nanoseconds = static_cast<std::int64_t>(
        (frameOffset * kNanosecondsPerSecond) / block.sampleRate);
    return block.timestamp + core::Nanoseconds{nanoseconds};
}

}  // namespace

AsyncTrackRecorder::AsyncTrackRecorder(
    AsyncTrackRecorderConfig config, std::unique_ptr<ITrackSegmentEncoder> encoder,
    std::unique_ptr<DurableSegmentPublisher> publisher,
    std::unique_ptr<DiskSpaceMonitor> diskSpaceMonitor)
    : config_(std::move(config)),
      encoder_(std::move(encoder)),
      publisher_(std::move(publisher)),
      diskSpaceMonitor_(std::move(diskSpaceMonitor)),
      requestedEndTime_(config_.recordingStartTime),
      lastMediaEndTime_(config_.recordingStartTime) {}

AsyncTrackRecorder::~AsyncTrackRecorder() {
    stopAsync(config_.recordingStartTime);
    if (worker_.joinable()) worker_.join();
}

core::Result<void> AsyncTrackRecorder::start() {
    std::lock_guard lock{mutex_};
    if (state_ != TrackRecorderState::Idle) {
        return invalidState("A track recorder instance cannot be restarted");
    }
    if (!encoder_ || !publisher_ || !diskSpaceMonitor_) {
        return invalidState("Track recorder dependencies are incomplete");
    }
    if (config_.segmentDuration <= core::DurationNs::zero() ||
        (config_.track.mediaKind() == TrackMediaKind::Video &&
         config_.videoQueueCapacity == 0) ||
        (config_.track.mediaKind() == TrackMediaKind::Audio &&
         config_.audioQueueFrameCapacity == 0)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Track recorder queue and segment limits must be positive"};
    }

    state_ = TrackRecorderState::Running;
    try {
        worker_ = std::thread{[this] { run(); }};
    } catch (const std::system_error& error) {
        state_ = TrackRecorderState::Idle;
        return core::AppError{core::ErrorCode::IoFailure,
                              "Could not start recording worker: " +
                                  std::string{error.what()}};
    }
    return core::ok();
}

core::Result<void> AsyncTrackRecorder::accept(media::VideoFrame frame) {
    if (config_.track.mediaKind() != TrackMediaKind::Video) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Video was submitted to an audio recording track"};
    }

    {
        std::lock_guard lock{mutex_};
        if (state_ != TrackRecorderState::Running) {
            if (inputError_) return *inputError_;
            return invalidState("Recording track is not accepting video");
        }
        if (videoQueue_.size() == config_.videoQueueCapacity) {
            videoQueue_.pop_front();
            ++summary_.videoFramesDropped;
        }
        videoQueue_.push_back(std::move(frame));
    }
    wakeWorker_.notify_one();
    return core::ok();
}

core::Result<void> AsyncTrackRecorder::accept(media::AudioBlock block) {
    if (config_.track.mediaKind() != TrackMediaKind::Audio) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Audio was submitted to a video recording track"};
    }
    if (block.sampleRate == 0 || block.channels == 0 || block.frameCount == 0 ||
        !block.samples) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Audio block format and samples must be valid"};
    }

    std::optional<core::AppError> overflow;
    {
        std::lock_guard lock{mutex_};
        if (state_ != TrackRecorderState::Running) {
            if (inputError_) return *inputError_;
            return invalidState("Recording track is not accepting audio");
        }
        if (block.frameCount > config_.audioQueueFrameCapacity - queuedAudioFrames_) {
            overflow = invalidState("Audio recording queue capacity was exhausted");
            inputError_ = *overflow;
            state_ = TrackRecorderState::Stopping;
            stopRequested_ = true;
            requestedEndTime_ = std::max(requestedEndTime_, block.timestamp);
        } else {
            queuedAudioFrames_ += block.frameCount;
            audioQueue_.push_back(std::move(block));
        }
    }
    wakeWorker_.notify_one();
    if (overflow) return *overflow;
    return core::ok();
}

void AsyncTrackRecorder::stopAsync(core::TimestampNs endTime, StopCompletion completion) {
    std::optional<core::Result<TrackRecordingSummary>> immediate;
    {
        std::lock_guard lock{mutex_};
        if (state_ == TrackRecorderState::Idle) {
            immediate = invalidState("Recording track has not been started");
        } else if (state_ == TrackRecorderState::Stopped) {
            immediate = finalResult_;
        } else {
            if (completion) stopCompletions_.push_back(std::move(completion));
            state_ = TrackRecorderState::Stopping;
            stopRequested_ = true;
            requestedEndTime_ = std::max(requestedEndTime_, endTime);
        }
    }
    wakeWorker_.notify_one();
    if (immediate && completion) completion(*immediate);
}

void AsyncTrackRecorder::observeCompletion(StopCompletion observer) {
    std::optional<core::Result<TrackRecordingSummary>> immediate;
    {
        std::lock_guard lock{mutex_};
        completionObserver_ = observer;
        if (completionObserver_ && state_ == TrackRecorderState::Stopped) {
            immediate = finalResult_;
        }
    }
    if (immediate && observer) observer(*immediate);
}

TrackRecorderSnapshot AsyncTrackRecorder::snapshot() const {
    std::lock_guard lock{mutex_};
    return TrackRecorderSnapshot{
        .state = state_,
        .queuedVideoFrames = videoQueue_.size(),
        .queuedAudioFrames = queuedAudioFrames_,
        .summary = summary_,
        .diskSpace = diskSpaceSnapshot_,
        .encoderName = encoderName_,
        .terminalError = inputError_,
    };
}

void AsyncTrackRecorder::run() {
    for (;;) {
        std::optional<media::VideoFrame> video;
        std::optional<media::AudioBlock> audio;
        {
            std::unique_lock lock{mutex_};
            wakeWorker_.wait(lock, [this] { return stopRequested_ || !queueEmptyLocked(); });
            if (config_.track.mediaKind() == TrackMediaKind::Video && !videoQueue_.empty()) {
                video = std::move(videoQueue_.front());
                videoQueue_.pop_front();
            } else if (config_.track.mediaKind() == TrackMediaKind::Audio &&
                       !audioQueue_.empty()) {
                audio = std::move(audioQueue_.front());
                queuedAudioFrames_ -= audio->frameCount;
                audioQueue_.pop_front();
            } else if (stopRequested_) {
                break;
            }
        }

        core::Result<void> processed = video ? process(*video) : process(*audio);
        if (!processed.hasValue()) {
            abortSegment();
            {
                std::lock_guard lock{mutex_};
                inputError_ = processed.error();
                videoQueue_.clear();
                audioQueue_.clear();
                queuedAudioFrames_ = 0;
            }
            complete(processed.error());
            return;
        }
    }

    core::TimestampNs finalEnd{};
    std::optional<core::AppError> inputError;
    {
        std::lock_guard lock{mutex_};
        finalEnd = std::max(requestedEndTime_, lastMediaEndTime_);
        inputError = inputError_;
    }
    if (segmentOpen_) {
        finalEnd = std::min(finalEnd, segmentEndTime_);
        if (auto finished = finishSegment(finalEnd); !finished.hasValue()) {
            complete(finished.error());
            return;
        }
    }
    if (inputError) {
        complete(*inputError);
        return;
    }
    TrackRecordingSummary summary;
    {
        std::lock_guard lock{mutex_};
        summary = summary_;
    }
    complete(summary);
}

core::Result<void> AsyncTrackRecorder::process(const media::VideoFrame& frame) {
    if (frame.timestamp < config_.recordingStartTime) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Video timestamp precedes the recording start"};
    }
    if (auto segment = ensureSegment(frame.timestamp); !segment.hasValue()) {
        return segment.error();
    }
    if (auto accepted = encoder_->accept(frame); !accepted.hasValue()) {
        return accepted.error();
    }
    lastMediaEndTime_ = std::max(lastMediaEndTime_, frame.timestamp + core::Nanoseconds{1});
    {
        std::lock_guard lock{mutex_};
        ++summary_.videoFramesAccepted;
    }
    return core::ok();
}

core::Result<void> AsyncTrackRecorder::process(const media::AudioBlock& block) {
    if (block.timestamp < config_.recordingStartTime) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Audio timestamp precedes the recording start"};
    }

    std::uint64_t offset = 0;
    while (offset < block.frameCount) {
        const auto timestamp = audioTimestamp(block, offset);
        if (auto segment = ensureSegment(timestamp); !segment.hasValue()) {
            return segment.error();
        }
        const auto untilBoundary = segmentEndTime_ - timestamp;
        const auto numerator = static_cast<std::uint64_t>(untilBoundary.count()) *
                               static_cast<std::uint64_t>(block.sampleRate);
        const auto framesToBoundary =
            std::max<std::uint64_t>(1, (numerator + kNanosecondsPerSecond - 1) /
                                           kNanosecondsPerSecond);
        const auto sliceFrames = std::min<std::uint64_t>(block.frameCount - offset,
                                                         framesToBoundary);
        const auto sampleOffset = offset * block.channels;
        std::shared_ptr<const float[]> sliceSamples{block.samples,
                                                    block.samples.get() + sampleOffset};
        media::AudioBlock slice{
            .timestamp = timestamp,
            .sampleRate = block.sampleRate,
            .channels = block.channels,
            .frameCount = static_cast<std::uint32_t>(sliceFrames),
            .samples = std::move(sliceSamples),
        };
        if (auto accepted = encoder_->accept(slice); !accepted.hasValue()) {
            return accepted.error();
        }
        offset += sliceFrames;
        lastMediaEndTime_ = std::max(lastMediaEndTime_, audioTimestamp(block, offset));
        {
            std::lock_guard lock{mutex_};
            summary_.audioFramesAccepted += sliceFrames;
        }
    }
    return core::ok();
}

core::Result<void> AsyncTrackRecorder::ensureSegment(core::TimestampNs timestamp) {
    if (segmentOpen_ && timestamp >= segmentEndTime_) {
        if (auto finished = finishSegment(segmentEndTime_); !finished.hasValue()) {
            return finished.error();
        }
    }
    if (segmentOpen_) return core::ok();

    const auto elapsed = timestamp - config_.recordingStartTime;
    const auto bucket = elapsed / config_.segmentDuration;
    const auto start = config_.recordingStartTime + config_.segmentDuration * bucket;
    return openSegment(start);
}

core::Result<void> AsyncTrackRecorder::openSegment(core::TimestampNs startTime) {
    auto space = diskSpaceMonitor_->check(config_.packageRoot,
                                          config_.nextSegmentEstimateBytes);
    if (!space.hasValue()) {
        return space.error();
    }
    {
        std::lock_guard lock{mutex_};
        diskSpaceSnapshot_ = space.value();
    }
    auto paths = publisher_->begin(config_.track, nextSegmentIndex_, startTime);
    if (!paths.hasValue()) return paths.error();

    SegmentEncodeConfig encodeConfig{
        .track = config_.track,
        .partPath = paths.value().partPath,
        .startTime = startTime,
        .targetDuration = config_.segmentDuration,
    };
    if (auto started = encoder_->start(encodeConfig); !started.hasValue()) {
        static_cast<void>(publisher_->fail());
        return started.error();
    }
    segmentOpen_ = true;
    segmentStartTime_ = startTime;
    segmentEndTime_ = startTime + config_.segmentDuration;
    return core::ok();
}

core::Result<void> AsyncTrackRecorder::finishSegment(core::TimestampNs endTime) {
    auto encoded = encoder_->finish(endTime);
    if (!encoded.hasValue()) {
        abortSegment();
        return encoded.error();
    }
    auto published = publisher_->publish(encoded.value());
    segmentOpen_ = false;
    if (!published.hasValue()) return published.error();

    {
        std::lock_guard lock{mutex_};
        ++summary_.segmentsPublished;
        summary_.bytesPublished += encoded.value().bytesWritten;
        encoderName_ = encoded.value().codecName;
    }
    ++nextSegmentIndex_;
    return core::ok();
}

void AsyncTrackRecorder::abortSegment() noexcept {
    if (!segmentOpen_) return;
    encoder_->abort();
    static_cast<void>(publisher_->fail());
    segmentOpen_ = false;
}

void AsyncTrackRecorder::complete(core::Result<TrackRecordingSummary> result) {
    std::vector<StopCompletion> completions;
    StopCompletion observer;
    {
        std::lock_guard lock{mutex_};
        if (state_ == TrackRecorderState::Stopped) return;
        state_ = TrackRecorderState::Stopped;
        stopRequested_ = true;
        finalResult_ = result;
        completions = std::move(stopCompletions_);
        observer = std::move(completionObserver_);
    }
    if (observer) observer(result);
    for (const auto& completion : completions) {
        if (completion) completion(result);
    }
}

bool AsyncTrackRecorder::queueEmptyLocked() const noexcept {
    return videoQueue_.empty() && audioQueue_.empty();
}

}  // namespace creator::recorder
