#pragma once

#include "core/Result.h"
#include "core/Utc.h"
#include "domain/EditCommand.h"
#include "domain/EditHistory.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "project_store/ITimelineStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace creator::app {

class TimelineEditService final {
public:
    using EventIdFactory = std::function<std::string()>;
    using Clock = std::function<core::Utc()>;

    [[nodiscard]] static core::Result<TimelineEditService> open(
        project_store::ITimelineStore& store, std::size_t historyLimit,
        EventIdFactory eventIdFactory, Clock clock);

    TimelineEditService(TimelineEditService&&) noexcept = default;
    TimelineEditService& operator=(TimelineEditService&&) noexcept = default;
    TimelineEditService(const TimelineEditService&) = delete;
    TimelineEditService& operator=(const TimelineEditService&) = delete;

    [[nodiscard]] core::Result<void> execute(
        std::unique_ptr<domain::IEditCommand> command);
    [[nodiscard]] core::Result<void> undo();
    [[nodiscard]] core::Result<void> redo();
    [[nodiscard]] core::Result<void> markExplicitSave();

    [[nodiscard]] const domain::Timeline& snapshot() const noexcept {
        return timeline_;
    }
    [[nodiscard]] std::int64_t revision() const noexcept {
        return revision_.value();
    }
    [[nodiscard]] bool canUndo() const noexcept { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return history_.canRedo(); }
    [[nodiscard]] std::size_t historyCursor() const noexcept {
        return history_.cursor();
    }
    [[nodiscard]] bool isClean() const noexcept { return history_.isClean(); }

private:
    TimelineEditService(project_store::ITimelineStore& store,
                        domain::Timeline timeline, domain::EditHistory history,
                        domain::TimelineRevision revision,
                        EventIdFactory eventIdFactory, Clock clock)
        : store_(&store),
          timeline_(std::move(timeline)),
          history_(std::move(history)),
          revision_(revision),
          eventIdFactory_(std::move(eventIdFactory)),
          clock_(std::move(clock)) {}

    [[nodiscard]] core::Result<void> persist(
        domain::Timeline stagedTimeline, domain::EditHistory stagedHistory,
        project_store::EditEventKind kind,
        domain::EditCommandRecord command);

    project_store::ITimelineStore* store_;
    domain::Timeline timeline_;
    domain::EditHistory history_;
    domain::TimelineRevision revision_;
    EventIdFactory eventIdFactory_;
    Clock clock_;
};

}  // namespace creator::app
