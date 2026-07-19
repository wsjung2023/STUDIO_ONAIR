#include "app/MultiTrackRecordingService.h"

#include "core/AppError.h"

#include <algorithm>
#include <utility>

namespace creator::app {
namespace {

core::AppError stateError(std::string message) {
    return {core::ErrorCode::InvalidState, std::move(message)};
}

}  // namespace

MultiTrackRecordingService::~MultiTrackRecordingService() {
    {
        std::lock_guard lock{mutex_};
        destroying_ = true;
        stopCompletions_.clear();
        completionObserver_ = {};
    }
    for (auto& entry : entries_) {
        if (entry.recorder) entry.recorder->observeCompletion({});
    }
    for (auto& entry : entries_) entry.recorder.reset();
}

core::Result<void> MultiTrackRecordingService::addTrack(
    std::unique_ptr<recorder::AsyncTrackRecorder> recorder) {
    if (!recorder) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Cannot add an empty recording track"};
    }
    std::lock_guard lock{mutex_};
    if (state_ != MultiTrackRecordingState::Idle) {
        return stateError("Recording tracks can only be added before start");
    }
    if (recorder->snapshot().state != recorder::TrackRecorderState::Idle) {
        return stateError("A recording service requires fresh track recorders");
    }
    const auto duplicate = std::find_if(entries_.begin(), entries_.end(),
                                        [&recorder](const Entry& entry) {
                                            return entry.recorder->track().sourceId() ==
                                                   recorder->track().sourceId();
                                        });
    if (duplicate != entries_.end()) {
        return core::AppError{core::ErrorCode::AlreadyExists,
                              "A source already has a recording track"};
    }
    entries_.push_back(Entry{.recorder = std::move(recorder),
                             .started = false,
                             .completed = false,
                             .summary = std::nullopt});
    return core::ok();
}

core::Result<void> MultiTrackRecordingService::start() {
    {
        std::lock_guard lock{mutex_};
        if (state_ != MultiTrackRecordingState::Idle) {
            return stateError("Multi-track recording cannot be restarted");
        }
        if (entries_.empty()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Multi-track recording requires at least one source"};
        }
        state_ = MultiTrackRecordingState::Running;
    }

    for (std::size_t index = 0; index < entries_.size(); ++index) {
        entries_[index].recorder->observeCompletion(
            [this, index](const auto& result) { onTrackCompleted(index, result); });
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto started = entries_[index].recorder->start();
        if (!started.hasValue()) {
            std::optional<CompletionDelivery> delivery;
            {
                std::lock_guard lock{mutex_};
                terminalError_ = started.error();
                state_ = MultiTrackRecordingState::Stopping;
                for (std::size_t unstarted = index; unstarted < entries_.size(); ++unstarted) {
                    entries_[unstarted].completed = true;
                }
                delivery = finishIfReadyLocked();
            }
            for (std::size_t active = 0; active < index; ++active) {
                entries_[active].recorder->stopAsync(core::TimestampNs{});
            }
            if (delivery) deliver(std::move(*delivery));
            return started.error();
        }
        std::lock_guard lock{mutex_};
        entries_[index].started = true;
    }
    return core::ok();
}

core::Result<void> MultiTrackRecordingService::accept(
    const domain::SourceId& sourceId, media::VideoFrame frame) {
    recorder::AsyncTrackRecorder* target = nullptr;
    {
        std::lock_guard lock{mutex_};
        if (state_ != MultiTrackRecordingState::Running) {
            if (terminalError_) return *terminalError_;
            return stateError("Multi-track recording is not accepting video");
        }
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [&sourceId](const Entry& entry) {
                                            return entry.recorder->track().sourceId() == sourceId;
                                        });
        if (found == entries_.end()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "Video source has no recording track"};
        }
        target = found->recorder.get();
    }
    const auto endTime = frame.timestamp;
    auto accepted = target->accept(std::move(frame));
    if (!accepted.hasValue()) latchFailure(accepted.error(), endTime);
    return accepted;
}

core::Result<void> MultiTrackRecordingService::accept(
    const domain::SourceId& sourceId, media::AudioBlock block) {
    recorder::AsyncTrackRecorder* target = nullptr;
    {
        std::lock_guard lock{mutex_};
        if (state_ != MultiTrackRecordingState::Running) {
            if (terminalError_) return *terminalError_;
            return stateError("Multi-track recording is not accepting audio");
        }
        const auto found = std::find_if(entries_.begin(), entries_.end(),
                                        [&sourceId](const Entry& entry) {
                                            return entry.recorder->track().sourceId() == sourceId;
                                        });
        if (found == entries_.end()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "Audio source has no recording track"};
        }
        target = found->recorder.get();
    }
    const auto endTime = block.timestamp;
    auto accepted = target->accept(std::move(block));
    if (!accepted.hasValue()) latchFailure(accepted.error(), endTime);
    return accepted;
}

void MultiTrackRecordingService::fail(core::AppError error,
                                      core::TimestampNs endTime) {
    latchFailure(error, endTime);
}

void MultiTrackRecordingService::stopAsync(core::TimestampNs endTime,
                                           StopCompletion completion) {
    std::vector<recorder::AsyncTrackRecorder*> toStop;
    std::optional<core::Result<MultiTrackRecordingSummary>> immediate;
    std::optional<CompletionDelivery> delivery;
    {
        std::lock_guard lock{mutex_};
        if (state_ == MultiTrackRecordingState::Idle) {
            immediate = stateError("Multi-track recording has not been started");
        } else if (state_ == MultiTrackRecordingState::Stopped) {
            immediate = finalResult_;
        } else {
            if (completion) stopCompletions_.push_back(std::move(completion));
            state_ = MultiTrackRecordingState::Stopping;
            for (auto& entry : entries_) {
                if (entry.started && !entry.completed) toStop.push_back(entry.recorder.get());
            }
            delivery = finishIfReadyLocked();
        }
    }
    for (auto* recorder : toStop) recorder->stopAsync(endTime);
    if (delivery) deliver(std::move(*delivery));
    if (immediate && completion) completion(*immediate);
}

void MultiTrackRecordingService::observeCompletion(StopCompletion observer) {
    std::optional<core::Result<MultiTrackRecordingSummary>> immediate;
    {
        std::lock_guard lock{mutex_};
        completionObserver_ = observer;
        if (completionObserver_ && state_ == MultiTrackRecordingState::Stopped) {
            immediate = finalResult_;
        }
    }
    if (immediate && observer) observer(*immediate);
}

MultiTrackRecordingSnapshot MultiTrackRecordingService::snapshot() const {
    std::lock_guard lock{mutex_};
    MultiTrackRecordingSnapshot result{.state = state_,
                                       .tracks = {},
                                       .terminalError = terminalError_};
    result.tracks.reserve(entries_.size());
    for (const auto& entry : entries_) {
        result.tracks.push_back({entry.recorder->track().sourceId(),
                                 entry.recorder->snapshot()});
    }
    return result;
}

void MultiTrackRecordingService::onTrackCompleted(
    std::size_t index,
    const core::Result<recorder::TrackRecordingSummary>& result) {
    std::vector<recorder::AsyncTrackRecorder*> toStop;
    std::optional<CompletionDelivery> delivery;
    {
        std::lock_guard lock{mutex_};
        if (destroying_ || index >= entries_.size() || entries_[index].completed) return;
        entries_[index].completed = true;
        if (result.hasValue()) entries_[index].summary = result.value();
        else {
            if (!terminalError_) terminalError_ = result.error();
            if (state_ == MultiTrackRecordingState::Running) {
                state_ = MultiTrackRecordingState::Stopping;
                for (auto& entry : entries_) {
                    if (entry.started && !entry.completed) {
                        toStop.push_back(entry.recorder.get());
                    }
                }
            }
        }
        delivery = finishIfReadyLocked();
    }
    const auto endTime = core::ProjectClock::now();
    for (auto* recorder : toStop) recorder->stopAsync(endTime);
    if (delivery) deliver(std::move(*delivery));
}

void MultiTrackRecordingService::latchFailure(const core::AppError& error,
                                              core::TimestampNs endTime) {
    std::vector<recorder::AsyncTrackRecorder*> toStop;
    {
        std::lock_guard lock{mutex_};
        if (!terminalError_) terminalError_ = error;
        if (state_ == MultiTrackRecordingState::Running) {
            state_ = MultiTrackRecordingState::Stopping;
            for (auto& entry : entries_) {
                if (entry.started && !entry.completed) toStop.push_back(entry.recorder.get());
            }
        }
    }
    for (auto* recorder : toStop) recorder->stopAsync(endTime);
}

std::optional<MultiTrackRecordingService::CompletionDelivery>
MultiTrackRecordingService::finishIfReadyLocked() {
    if (state_ != MultiTrackRecordingState::Stopping ||
        std::any_of(entries_.begin(), entries_.end(),
                    [](const Entry& entry) { return !entry.completed; })) {
        return std::nullopt;
    }

    core::Result<MultiTrackRecordingSummary> result = [&]()
        -> core::Result<MultiTrackRecordingSummary> {
        if (terminalError_) return *terminalError_;
        MultiTrackRecordingSummary summary;
        summary.tracks.reserve(entries_.size());
        for (const auto& entry : entries_) {
            if (entry.summary) {
                summary.tracks.push_back(
                    {entry.recorder->track().sourceId(), *entry.summary});
            }
        }
        return summary;
    }();
    state_ = MultiTrackRecordingState::Stopped;
    finalResult_ = result;
    return CompletionDelivery{.result = std::move(result),
                              .completions = std::move(stopCompletions_),
                              .observer = std::move(completionObserver_)};
}

void MultiTrackRecordingService::deliver(CompletionDelivery delivery) {
    if (delivery.observer) delivery.observer(delivery.result);
    for (const auto& completion : delivery.completions) {
        if (completion) completion(delivery.result);
    }
}

}  // namespace creator::app
