#include "app/TimelineEditService.h"

#include "core/AppError.h"

#include <utility>

namespace creator::app {

core::Result<TimelineEditService> TimelineEditService::open(
    project_store::ITimelineStore& store, std::size_t historyLimit,
    EventIdFactory eventIdFactory, Clock clock) {
    if (historyLimit == 0 || !eventIdFactory || !clock) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "timeline edit service dependencies are invalid"};
    }
    auto session = store.loadEditSession(historyLimit);
    if (!session.hasValue()) return session.error();
    auto revision = domain::TimelineRevision::create(
        session.value().persisted.revision);
    if (!revision.hasValue()) return revision.error();
    return TimelineEditService{
        store, std::move(session.value().persisted.timeline),
        std::move(session.value().history), revision.value(),
        std::move(eventIdFactory), std::move(clock)};
}

core::Result<void> TimelineEditService::execute(
    std::unique_ptr<domain::IEditCommand> command) {
    domain::Timeline stagedTimeline = timeline_;
    domain::EditHistory stagedHistory = history_;
    if (auto result = stagedHistory.execute(stagedTimeline, std::move(command));
        !result.hasValue()) {
        return result.error();
    }
    auto record = stagedHistory.undoRecord();
    if (!record.has_value()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "executed command is absent from edit history"};
    }
    return persist(std::move(stagedTimeline), std::move(stagedHistory),
                   project_store::EditEventKind::Apply, std::move(*record));
}

core::Result<void> TimelineEditService::undo() {
    domain::Timeline stagedTimeline = timeline_;
    domain::EditHistory stagedHistory = history_;
    auto record = stagedHistory.undoRecord();
    if (!record.has_value()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "edit history has nothing to undo"};
    }
    if (auto result = stagedHistory.undo(stagedTimeline); !result.hasValue()) {
        return result.error();
    }
    return persist(std::move(stagedTimeline), std::move(stagedHistory),
                   project_store::EditEventKind::Undo, std::move(*record));
}

core::Result<void> TimelineEditService::redo() {
    domain::Timeline stagedTimeline = timeline_;
    domain::EditHistory stagedHistory = history_;
    auto record = stagedHistory.redoRecord();
    if (!record.has_value()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "edit history has nothing to redo"};
    }
    if (auto result = stagedHistory.redo(stagedTimeline); !result.hasValue()) {
        return result.error();
    }
    return persist(std::move(stagedTimeline), std::move(stagedHistory),
                   project_store::EditEventKind::Redo, std::move(*record));
}

core::Result<void> TimelineEditService::markExplicitSave() {
    if (auto saved = store_->markExplicitSave(
            timeline_.id(), revision_.value(), history_.cursor());
        !saved.hasValue()) {
        return saved.error();
    }
    history_.markClean();
    return core::ok();
}

core::Result<void> TimelineEditService::persist(
    domain::Timeline stagedTimeline, domain::EditHistory stagedHistory,
    project_store::EditEventKind kind, domain::EditCommandRecord command) {
    const std::string eventId = eventIdFactory_();
    if (eventId.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "edit event identity must not be empty"};
    }
    project_store::TimelineCommit commit{
        .snapshot = stagedTimeline,
        .expectedRevision = revision_.value(),
        .event = project_store::EditEventRecord{
            .eventId = eventId,
            .kind = kind,
            .command = std::move(command),
            .createdAt = clock_()},
        .historyCount = stagedHistory.size(),
        .historyCursor = stagedHistory.cursor(),
        .cleanCursor = stagedHistory.cleanCursor()};
    if (auto committed = store_->commitEdit(commit); !committed.hasValue()) {
        return committed.error();
    }
    auto next = revision_.next();
    if (!next.hasValue()) return next.error();
    timeline_ = std::move(stagedTimeline);
    history_ = std::move(stagedHistory);
    revision_ = next.value();
    return core::ok();
}

}  // namespace creator::app
