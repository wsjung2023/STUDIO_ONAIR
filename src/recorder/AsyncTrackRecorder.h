#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "media/MediaTypes.h"
#include "recorder/DiskSpaceMonitor.h"
#include "recorder/DurableSegmentPublisher.h"
#include "recorder/RecordingTrack.h"
#include "recorder/TrackSegmentPorts.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <condition_variable>

namespace creator::recorder {

enum class TrackRecorderState { Idle, Running, Stopping, Stopped };

struct AsyncTrackRecorderConfig final {
    RecordingTrack track;
    std::filesystem::path packageRoot;
    core::TimestampNs recordingStartTime{};
    core::DurationNs segmentDuration{std::chrono::seconds{2}};
    std::size_t videoQueueCapacity{3};
    std::uint64_t audioQueueFrameCapacity{48'000};
    std::uint64_t nextSegmentEstimateBytes{64ULL * 1024ULL * 1024ULL};
};

struct TrackRecordingSummary final {
    std::uint64_t segmentsPublished{0};
    std::uint64_t bytesPublished{0};
    std::uint64_t videoFramesAccepted{0};
    std::uint64_t audioFramesAccepted{0};
    std::uint64_t videoFramesDropped{0};
};

struct TrackRecorderSnapshot final {
    TrackRecorderState state{TrackRecorderState::Idle};
    std::size_t queuedVideoFrames{0};
    std::uint64_t queuedAudioFrames{0};
    TrackRecordingSummary summary;
    std::optional<core::AppError> terminalError;
};

class AsyncTrackRecorder final {
public:
    using StopCompletion =
        std::function<void(const core::Result<TrackRecordingSummary>&)>;

    AsyncTrackRecorder(AsyncTrackRecorderConfig config,
                       std::unique_ptr<ITrackSegmentEncoder> encoder,
                       std::unique_ptr<DurableSegmentPublisher> publisher,
                       std::unique_ptr<DiskSpaceMonitor> diskSpaceMonitor);
    ~AsyncTrackRecorder();

    AsyncTrackRecorder(const AsyncTrackRecorder&) = delete;
    AsyncTrackRecorder& operator=(const AsyncTrackRecorder&) = delete;

    [[nodiscard]] core::Result<void> start();
    [[nodiscard]] core::Result<void> accept(media::VideoFrame frame);
    [[nodiscard]] core::Result<void> accept(media::AudioBlock block);
    void stopAsync(core::TimestampNs endTime, StopCompletion completion = {});
    void observeCompletion(StopCompletion observer);
    [[nodiscard]] TrackRecorderSnapshot snapshot() const;
    [[nodiscard]] const RecordingTrack& track() const noexcept { return config_.track; }

private:
    void run();
    [[nodiscard]] core::Result<void> process(const media::VideoFrame& frame);
    [[nodiscard]] core::Result<void> process(const media::AudioBlock& block);
    [[nodiscard]] core::Result<void> ensureSegment(core::TimestampNs timestamp);
    [[nodiscard]] core::Result<void> openSegment(core::TimestampNs startTime);
    [[nodiscard]] core::Result<void> finishSegment(core::TimestampNs endTime);
    void abortSegment() noexcept;
    void complete(core::Result<TrackRecordingSummary> result);
    [[nodiscard]] bool queueEmptyLocked() const noexcept;

    AsyncTrackRecorderConfig config_;
    std::unique_ptr<ITrackSegmentEncoder> encoder_;
    std::unique_ptr<DurableSegmentPublisher> publisher_;
    std::unique_ptr<DiskSpaceMonitor> diskSpaceMonitor_;

    mutable std::mutex mutex_;
    std::condition_variable wakeWorker_;
    std::deque<media::VideoFrame> videoQueue_;
    std::deque<media::AudioBlock> audioQueue_;
    std::uint64_t queuedAudioFrames_{0};
    TrackRecorderState state_{TrackRecorderState::Idle};
    bool stopRequested_{false};
    core::TimestampNs requestedEndTime_{};
    std::optional<core::AppError> inputError_;
    std::optional<core::Result<TrackRecordingSummary>> finalResult_;
    std::vector<StopCompletion> stopCompletions_;
    StopCompletion completionObserver_;
    TrackRecordingSummary summary_;
    std::thread worker_;

    bool segmentOpen_{false};
    std::uint64_t nextSegmentIndex_{0};
    core::TimestampNs segmentStartTime_{};
    core::TimestampNs segmentEndTime_{};
    core::TimestampNs lastMediaEndTime_{};
};

}  // namespace creator::recorder
