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

/// A source stopped delivering mid-take (device unplugged, audio endpoint
/// invalidated, capture error). Nothing is stopped for it: the track stays
/// open so the source can resume; the dropout is kept for the UI and log.
struct SourceDropout final {
    domain::SourceId sourceId;
    core::AppError error;
    core::TimestampNs at{};
};

struct CompletedTrackRecording final {
    domain::SourceId sourceId;
    recorder::TrackRecordingSummary summary;
    /// Why this track ended before the take did (its own encoder/disk fault).
    std::optional<core::AppError> failure;
};

struct MultiTrackRecordingSummary final {
    std::vector<CompletedTrackRecording> tracks;
    std::vector<SourceDropout> dropouts;
};

struct ObservedTrackRecording final {
    domain::SourceId sourceId;
    recorder::TrackRecorderSnapshot snapshot;
    std::optional<core::AppError> failure;
};

struct MultiTrackRecordingSnapshot final {
    MultiTrackRecordingState state{MultiTrackRecordingState::Idle};
    std::vector<ObservedTrackRecording> tracks;
    std::vector<SourceDropout> dropouts;
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
    /// Terminates every active track after a TAKE-WIDE fault (clock/sync or
    /// planning failure). A fault of one track or one source is not take-wide:
    /// see accept() (per-track failure) and noteSourceDropout().
    /// The first failure remains the terminal diagnostic; later duplicates
    /// are harmless and do not trigger a second completion.
    void fail(core::AppError error, core::TimestampNs endTime);
    /// Records that a source stopped delivering. Keeps every track running.
    void noteSourceDropout(const domain::SourceId& sourceId, core::AppError error,
                           core::TimestampNs at);
    void stopAsync(core::TimestampNs endTime, StopCompletion completion = {});
    void observeCompletion(StopCompletion observer);
    [[nodiscard]] MultiTrackRecordingSnapshot snapshot() const;

private:
    struct Entry final {
        std::unique_ptr<recorder::AsyncTrackRecorder> recorder;
        bool started{false};
        bool completed{false};
        std::optional<recorder::TrackRecordingSummary> summary;
        std::optional<core::AppError> failure;
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
    /// Ends one track after its own fault; the take continues on the others
    /// and only becomes terminal when no live track is left.
    void failTrack(std::size_t index, const core::AppError& error,
                   core::TimestampNs endTime);
    [[nodiscard]] bool anyLiveTrackLocked() const noexcept;
    [[nodiscard]] std::optional<CompletionDelivery> finishIfReadyLocked();
    static void deliver(CompletionDelivery delivery);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::vector<SourceDropout> dropouts_;
    MultiTrackRecordingState state_{MultiTrackRecordingState::Idle};
    std::optional<core::AppError> terminalError_;
    std::optional<core::Result<MultiTrackRecordingSummary>> finalResult_;
    std::vector<StopCompletion> stopCompletions_;
    StopCompletion completionObserver_;
    bool destroying_{false};
};

}  // namespace creator::app
