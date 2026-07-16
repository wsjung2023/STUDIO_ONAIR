#pragma once

#include "core/Result.h"
#include "core/Utc.h"
#include "domain/EditCommand.h"
#include "domain/EditHistory.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace creator::project_store {

enum class EditEventKind { Apply, Undo, Redo };

struct EditEventRecord final {
    std::string eventId;
    EditEventKind kind;
    domain::EditCommandRecord command;
    core::Utc createdAt;

    friend bool operator==(const EditEventRecord&, const EditEventRecord&) = default;
};

struct PersistedTimeline final {
    domain::Timeline timeline;
    std::int64_t revision;
    std::size_t historyCount;
    std::size_t historyCursor;
    std::optional<std::size_t> cleanCursor;
    std::int64_t explicitSavedRevision;
    std::vector<EditEventRecord> events;

    friend bool operator==(const PersistedTimeline&, const PersistedTimeline&) = default;
};

struct TimelineCommit final {
    domain::Timeline snapshot;
    std::int64_t expectedRevision;
    EditEventRecord event;
    std::size_t historyCount;
    std::size_t historyCursor;
    std::optional<std::size_t> cleanCursor;
};

class ITimelineStore {
public:
    [[nodiscard]] virtual core::Result<void> putAsset(
        const domain::MediaAsset& asset) = 0;
    [[nodiscard]] virtual core::Result<domain::MediaAsset> asset(
        const domain::AssetId& assetId) = 0;
    [[nodiscard]] virtual core::Result<std::vector<domain::MediaAsset>> assets() = 0;
    [[nodiscard]] virtual core::Result<void> createTimeline(
        const domain::Timeline& timeline) = 0;
    [[nodiscard]] virtual core::Result<PersistedTimeline> loadPrimaryTimeline() = 0;
    [[nodiscard]] virtual core::Result<domain::EditHistory> loadEditHistory(
        std::size_t limit) = 0;
    [[nodiscard]] virtual core::Result<void> commitEdit(
        const TimelineCommit& commit) = 0;
    [[nodiscard]] virtual core::Result<void> markExplicitSave(
        const domain::TimelineId& timelineId, std::int64_t expectedRevision,
        std::size_t historyCursor) = 0;
    virtual ~ITimelineStore() = default;
};

}  // namespace creator::project_store
