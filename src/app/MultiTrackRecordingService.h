#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "media/MediaTypes.h"
#include "recorder/AsyncTrackRecorder.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace creator::app {

enum class MultiTrackRecordingState { Idle, Running, Stopping, Stopped };

struct CompletedTrackRecording final {
    domain::SourceId sourceId;
    recorder::TrackRecordingSummary summary;
};

struct MultiTrackRecordingSummary final {
    std::vector<CompletedTrackRecording> tracks;
};

struct ObservedTrackRecording final {
    domain::SourceId sourceId;
    recorder::TrackRecorderSnapshot snapshot;
};

struct MultiTrackRecordingSnapshot final {
    MultiTrackRecordingState state{MultiTrackRecordingState::Idle};
    std::vector<ObservedTrackRecording> tracks;
    std::optional<core::AppError> terminalError;
};

class MultiTrackRecordingService final {
public:
    using StopCompletion =
        std::function<void(const core::Result<MultiTrackRecordingSummary>&)>;

    MultiTrackRecordingService() = default;
    ~MultiTrackRecordingService();
    MultiTrackRecordingService(const MultiTrackRecordingService&) = delete;
    MultiTrackRecordingService& operator=(const MultiTrackRecordingService&) = delete;

    [[nodiscard]] core::Result<void> addTrack(
        std::unique_ptr<recorder::AsyncTrackRecorder> recorder);
    [[nodiscard]] core::Result<void> start();
    [[nodiscard]] core::Result<void> accept(
        const domain::SourceId& sourceId, media::VideoFrame frame);
    [[nodiscard]] core::Result<void> accept(
        const domain::SourceId& sourceId, media::AudioBlock block);
    void stopAsync(core::TimestampNs endTime, StopCompletion completion = {});
    void observeCompletion(StopCompletion observer);
    [[nodiscard]] MultiTrackRecordingSnapshot snapshot() const;

private:
    struct Entry final {
        std::unique_ptr<recorder::AsyncTrackRecorder> recorder;
        bool started{false};
        bool completed{false};
        std::optional<recorder::TrackRecordingSummary> summary;
    };

    struct CompletionDelivery final {
        core::Result<MultiTrackRecordingSummary> result;
        std::vector<StopCompletion> completions;
        StopCompletion observer;
    };

    void onTrackCompleted(
        std::size_t index,
        const core::Result<recorder::TrackRecordingSummary>& result);
    void latchFailure(const core::AppError& error, core::TimestampNs endTime);
    [[nodiscard]] std::optional<CompletionDelivery> finishIfReadyLocked();
    static void deliver(CompletionDelivery delivery);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    MultiTrackRecordingState state_{MultiTrackRecordingState::Idle};
    std::optional<core::AppError> terminalError_;
    std::optional<core::Result<MultiTrackRecordingSummary>> finalResult_;
    std::vector<StopCompletion> stopCompletions_;
    StopCompletion completionObserver_;
    bool destroying_{false};
};

}  // namespace creator::app
