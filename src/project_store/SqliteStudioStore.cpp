#include "project_store/SqliteStudioStore.h"

#include "core/AppError.h"
#include "core/Utc.h"
#include "project_store/MigrationRunner.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace creator::project_store {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::SceneId;
using domain::SceneSource;
using domain::SessionId;
using domain::SourceId;
using domain::StudioScene;
using domain::StudioSourceRole;
using domain::TimelineId;
using domain::VisualTransform;
using internal::SqliteStatement;
using internal::SqliteStep;
using internal::SqliteTransaction;

AppError corrupt(std::string message) {
    return AppError{ErrorCode::IoFailure,
                    "sqlite studio data is invalid: " + std::move(message)};
}

Result<void> expectDone(SqliteStatement& statement, std::string_view operation) {
    auto stepped = statement.step();
    if (!stepped.hasValue()) return stepped.error();
    if (stepped.value() != SqliteStep::Done) {
        return AppError{ErrorCode::IoFailure,
                        "sqlite " + std::string{operation} +
                            " unexpectedly returned a row"};
    }
    return core::ok();
}

Result<std::int64_t> sqlInteger(std::uint64_t value, std::string_view field) {
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
        return AppError{ErrorCode::InvalidArgument,
                        std::string{field} + " exceeds SQLite int64"};
    }
    return static_cast<std::int64_t>(value);
}

Result<std::int32_t> int32Value(std::int64_t value, std::string_view field) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        return corrupt(std::string{field} + " is outside int32");
    }
    return static_cast<std::int32_t>(value);
}

Result<void> bindOptionalDouble(SqliteStatement& statement, int index,
                                const std::optional<VisualTransform>& transform,
                                double (VisualTransform::*getter)() const noexcept) {
    if (!transform.has_value()) return statement.bindNull(index);
    return statement.bindDouble(index, ((*transform).*getter)());
}

Result<void> bindTransform(SqliteStatement& statement,
                           const std::optional<VisualTransform>& transform) {
    if (auto bound = bindOptionalDouble(statement, 7, transform,
                                        &VisualTransform::x);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 8, transform,
                                        &VisualTransform::y);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 9, transform,
                                        &VisualTransform::width);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 10, transform,
                                        &VisualTransform::height);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 11, transform,
                                        &VisualTransform::scaleX);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 12, transform,
                                        &VisualTransform::scaleY);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 13, transform,
                                        &VisualTransform::rotationDegrees);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 14, transform,
                                        &VisualTransform::cropLeft);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 15, transform,
                                        &VisualTransform::cropTop);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 16, transform,
                                        &VisualTransform::cropRight);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 17, transform,
                                        &VisualTransform::cropBottom);
        !bound.hasValue()) return bound.error();
    if (auto bound = bindOptionalDouble(statement, 18, transform,
                                        &VisualTransform::opacity);
        !bound.hasValue()) return bound.error();
    if (transform.has_value()) {
        return statement.bindInt64(19, transform->zOrder());
    }
    return statement.bindNull(19);
}

Result<std::optional<VisualTransform>> readTransform(SqliteStatement& statement) {
    if (statement.columnIsNull(5)) return std::optional<VisualTransform>{};
    for (int column = 5; column <= 17; ++column) {
        if (statement.columnIsNull(column)) return corrupt("partial visual transform");
    }
    auto zOrder = int32Value(statement.columnInt64(17), "visual z-order");
    if (!zOrder.hasValue()) return zOrder.error();
    auto transform = VisualTransform::create(
        statement.columnDouble(5), statement.columnDouble(6),
        statement.columnDouble(7), statement.columnDouble(8),
        statement.columnDouble(9), statement.columnDouble(10),
        statement.columnDouble(11), statement.columnDouble(12),
        statement.columnDouble(13), statement.columnDouble(14),
        statement.columnDouble(15), statement.columnDouble(16), zOrder.value());
    if (!transform.hasValue()) return corrupt("visual transform is invalid");
    return std::optional<VisualTransform>{std::move(transform).value()};
}

}  // namespace

Result<SqliteStudioStore> SqliteStudioStore::open(
    const std::filesystem::path& databasePath,
    const domain::ProjectId& expectedProjectId) {
    auto opened = internal::SqliteConnection::open(databasePath);
    if (!opened.hasValue()) return opened.error();
    auto connection = std::move(opened).value();
    if (auto migrated = MigrationRunner::apply(connection); !migrated.hasValue()) {
        return migrated.error();
    }
    auto prepared = connection.prepare(
        "SELECT project_id FROM projects WHERE project_id=?1");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, expectedProjectId.value());
        !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound, "studio project was not found"};
    }
    return SqliteStudioStore{std::move(connection), expectedProjectId};
}

Result<void> SqliteStudioStore::writeScene(const StudioScene& scene,
                                           std::string_view createdAtUtc) {
    auto sceneInsert = connection_.prepare(
        "INSERT INTO scenes(scene_id,project_id,name,position,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5)");
    if (!sceneInsert.hasValue()) return sceneInsert.error();
    auto sceneStatement = std::move(sceneInsert).value();
    if (auto value = sceneStatement.bindText(1, scene.id().value()); !value.hasValue()) return value.error();
    if (auto value = sceneStatement.bindText(2, projectId_.value()); !value.hasValue()) return value.error();
    if (auto value = sceneStatement.bindText(3, scene.name()); !value.hasValue()) return value.error();
    if (auto value = sceneStatement.bindInt64(4, scene.position()); !value.hasValue()) return value.error();
    if (auto value = sceneStatement.bindText(5, createdAtUtc); !value.hasValue()) return value.error();
    if (auto done = expectDone(sceneStatement, "scene insert"); !done.hasValue()) return done.error();

    return writeSceneSources(scene);
}

Result<void> SqliteStudioStore::writeSceneSources(const StudioScene& scene) {
    for (const auto& source : scene.sources()) {
        auto sourceInsert = connection_.prepare(
            "INSERT INTO scene_sources(scene_id,source_id,role,name,position,enabled,"
            "transform_x,transform_y,transform_width,transform_height,scale_x,scale_y,"
            "rotation_degrees,crop_left,crop_top,crop_right,crop_bottom,opacity,z_order) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)");
        if (!sourceInsert.hasValue()) return sourceInsert.error();
        auto sourceStatement = std::move(sourceInsert).value();
        if (auto value = sourceStatement.bindText(1, scene.id().value()); !value.hasValue()) return value.error();
        if (auto value = sourceStatement.bindText(2, source.id().value()); !value.hasValue()) return value.error();
        if (auto value = sourceStatement.bindText(3, domain::studioSourceRoleName(source.role())); !value.hasValue()) return value.error();
        if (auto value = sourceStatement.bindText(4, source.name()); !value.hasValue()) return value.error();
        if (auto value = sourceStatement.bindInt64(5, source.position()); !value.hasValue()) return value.error();
        if (auto value = sourceStatement.bindInt64(6, source.enabled() ? 1 : 0); !value.hasValue()) return value.error();
        if (auto value = bindTransform(sourceStatement, source.transform()); !value.hasValue()) return value.error();
        if (auto done = expectDone(sourceStatement, "scene source insert"); !done.hasValue()) return done.error();
    }
    return core::ok();
}

Result<void> SqliteStudioStore::seedDefaultsIfEmpty(
    const std::vector<StudioScene>& scenes) {
    if (scenes.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        "default studio scenes must not be empty"};
    }
    auto count = connection_.prepare(
        "SELECT count(*) FROM scenes WHERE project_id=?1");
    if (!count.hasValue()) return count.error();
    auto countStatement = std::move(count).value();
    if (auto bound = countStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = countStatement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) return corrupt("scene count returned no row");
    if (countStatement.columnInt64(0) != 0) {
        auto loaded = load();
        return loaded.hasValue() ? core::ok() : Result<void>{loaded.error()};
    }
    return commitSceneMutation(StudioSnapshot{.scenes = scenes,
                                              .activeSceneId = scenes.front().id()});
}

Result<StudioSnapshot> SqliteStudioStore::load() {
    auto activeQuery = connection_.prepare(
        "SELECT active_scene_id FROM studio_state WHERE project_id=?1");
    if (!activeQuery.hasValue()) return activeQuery.error();
    auto activeStatement = std::move(activeQuery).value();
    if (auto bound = activeStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    auto activeRow = activeStatement.step();
    if (!activeRow.hasValue()) return activeRow.error();
    if (activeRow.value() != SqliteStep::Row) return corrupt("active scene is missing");
    auto activeId = SceneId::create(activeStatement.columnText(0));
    if (!activeId.hasValue()) return corrupt("active scene id is empty");

    auto scenesQuery = connection_.prepare(
        "SELECT scene_id,name,position FROM scenes WHERE project_id=?1 "
        "ORDER BY position,scene_id");
    if (!scenesQuery.hasValue()) return scenesQuery.error();
    auto scenesStatement = std::move(scenesQuery).value();
    if (auto bound = scenesStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    std::vector<StudioScene> scenes;
    while (true) {
        auto sceneRow = scenesStatement.step();
        if (!sceneRow.hasValue()) return sceneRow.error();
        if (sceneRow.value() == SqliteStep::Done) break;
        auto sceneId = SceneId::create(scenesStatement.columnText(0));
        if (!sceneId.hasValue()) return corrupt("scene id is empty");
        auto position = int32Value(scenesStatement.columnInt64(2), "scene position");
        if (!position.hasValue()) return position.error();

        auto sourcesQuery = connection_.prepare(
            "SELECT source_id,role,name,position,enabled,transform_x,transform_y,"
            "transform_width,transform_height,scale_x,scale_y,rotation_degrees,"
            "crop_left,crop_top,crop_right,crop_bottom,opacity,z_order "
            "FROM scene_sources WHERE scene_id=?1 ORDER BY position,source_id");
        if (!sourcesQuery.hasValue()) return sourcesQuery.error();
        auto sourcesStatement = std::move(sourcesQuery).value();
        if (auto bound = sourcesStatement.bindText(1, sceneId.value().value()); !bound.hasValue()) return bound.error();
        std::vector<SceneSource> sources;
        while (true) {
            auto sourceRow = sourcesStatement.step();
            if (!sourceRow.hasValue()) return sourceRow.error();
            if (sourceRow.value() == SqliteStep::Done) break;
            auto sourceId = SourceId::create(sourcesStatement.columnText(0));
            auto role = domain::studioSourceRoleFromName(sourcesStatement.columnText(1));
            auto sourcePosition = int32Value(sourcesStatement.columnInt64(3), "source position");
            auto transform = readTransform(sourcesStatement);
            if (!sourceId.hasValue() || !role.hasValue() ||
                !sourcePosition.hasValue() || !transform.hasValue()) {
                return corrupt("scene source row cannot be decoded");
            }
            const auto enabledInteger = sourcesStatement.columnInt64(4);
            if (enabledInteger != 0 && enabledInteger != 1) return corrupt("source enabled is not boolean");
            auto source = SceneSource::create(
                std::move(sourceId).value(), role.value(),
                sourcesStatement.columnText(2), sourcePosition.value(),
                enabledInteger == 1, std::move(transform).value());
            if (!source.hasValue()) return corrupt("scene source violates domain bounds");
            sources.push_back(std::move(source).value());
        }
        auto scene = StudioScene::create(std::move(sceneId).value(),
                                         scenesStatement.columnText(1),
                                         position.value(), std::move(sources));
        if (!scene.hasValue()) return corrupt("scene violates domain bounds");
        scenes.push_back(std::move(scene).value());
    }
    if (scenes.empty() || std::ranges::find(scenes, activeId.value(),
                                            &StudioScene::id) == scenes.end()) {
        return corrupt("studio snapshot is empty or active scene is absent");
    }
    return StudioSnapshot{.scenes = std::move(scenes),
                          .activeSceneId = std::move(activeId).value()};
}

Result<void> SqliteStudioStore::commitSceneMutation(
    const StudioSnapshot& snapshot) {
    if (snapshot.scenes.empty() ||
        std::ranges::find(snapshot.scenes, snapshot.activeSceneId,
                          &StudioScene::id) == snapshot.scenes.end()) {
        return AppError{ErrorCode::InvalidArgument,
                        "studio snapshot needs an active scene"};
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::int32_t> positions;
    for (const auto& scene : snapshot.scenes) {
        if (!ids.insert(scene.id().value()).second ||
            !positions.insert(scene.position()).second) {
            return AppError{ErrorCode::InvalidArgument,
                            "studio scene identities and positions must be unique"};
        }
    }

    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    struct ExistingScene final {
        std::int32_t position;
    };
    std::unordered_map<std::string, ExistingScene> existing;
    auto timestamps = connection_.prepare(
        "SELECT scene_id,position FROM scenes WHERE project_id=?1");
    if (!timestamps.hasValue()) return timestamps.error();
    auto timestampStatement = std::move(timestamps).value();
    if (auto bound = timestampStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    while (true) {
        auto row = timestampStatement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto existingPosition = int32Value(timestampStatement.columnInt64(1),
                                           "stored scene position");
        if (!existingPosition.hasValue()) return existingPosition.error();
        existing.emplace(timestampStatement.columnText(0),
                         ExistingScene{.position = existingPosition.value()});
    }

    std::unordered_map<std::string, std::vector<SceneSource>> existingSources;
    if (!existing.empty()) {
        auto loaded = load();
        if (!loaded.hasValue()) return loaded.error();
        for (const auto& scene : loaded.value().scenes) {
            existingSources.emplace(scene.id().value(), scene.sources());
        }
    }
    auto clearState = connection_.prepare(
        "DELETE FROM studio_state WHERE project_id=?1");
    if (!clearState.hasValue()) return clearState.error();
    auto clearStateStatement = std::move(clearState).value();
    if (auto bound = clearStateStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(clearStateStatement, "studio state delete"); !done.hasValue()) return done.error();
    std::unordered_map<std::int32_t, std::string> sceneAtPosition;
    for (const auto& [id, value] : existing) {
        sceneAtPosition.emplace(value.position, id);
    }

    for (auto iterator = existing.begin(); iterator != existing.end();) {
        if (ids.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        auto removed = connection_.prepare(
            "DELETE FROM scenes WHERE project_id=?1 AND scene_id=?2");
        if (!removed.hasValue()) return removed.error();
        auto statement = std::move(removed).value();
        if (auto bound = statement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
        if (auto bound = statement.bindText(2, iterator->first); !bound.hasValue()) return bound.error();
        if (auto done = expectDone(statement, "removed scene delete"); !done.hasValue()) return done.error();
        sceneAtPosition.erase(iterator->second.position);
        iterator = existing.erase(iterator);
    }

    const auto moveScene = [this](std::string_view sceneId,
                                  std::int32_t position) -> Result<void> {
        auto updated = connection_.prepare(
            "UPDATE scenes SET position=?1 WHERE project_id=?2 AND scene_id=?3");
        if (!updated.hasValue()) return updated.error();
        auto statement = std::move(updated).value();
        if (auto bound = statement.bindInt64(1, position); !bound.hasValue()) return bound.error();
        if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
        if (auto bound = statement.bindText(3, sceneId); !bound.hasValue()) return bound.error();
        return expectDone(statement, "scene reorder");
    };

    for (const auto& scene : snapshot.scenes) {
        auto current = existing.find(scene.id().value());
        if (current == existing.end() || current->second.position == scene.position()) {
            continue;
        }
        const auto occupied = sceneAtPosition.find(scene.position());
        if (occupied != sceneAtPosition.end() && occupied->second != scene.id().value()) {
            std::optional<std::int32_t> freePosition;
            for (std::int32_t candidate = 0; candidate <= 1023; ++candidate) {
                if (!sceneAtPosition.contains(candidate) &&
                    !positions.contains(candidate)) {
                    freePosition = candidate;
                    break;
                }
            }
            if (!freePosition.has_value()) {
                for (std::int32_t candidate = 0; candidate <= 1023; ++candidate) {
                    if (!sceneAtPosition.contains(candidate)) {
                        freePosition = candidate;
                        break;
                    }
                }
            }
            if (!freePosition.has_value()) {
                return AppError{ErrorCode::InvalidState,
                                "studio scene reorder needs one free position"};
            }
            const auto displacedId = occupied->second;
            if (auto moved = moveScene(displacedId, *freePosition); !moved.hasValue()) return moved.error();
            auto displaced = existing.find(displacedId);
            sceneAtPosition.erase(occupied);
            displaced->second.position = *freePosition;
            sceneAtPosition.emplace(*freePosition, displacedId);
        }
        sceneAtPosition.erase(current->second.position);
        if (auto moved = moveScene(scene.id().value(), scene.position()); !moved.hasValue()) return moved.error();
        current->second.position = scene.position();
        sceneAtPosition[scene.position()] = scene.id().value();
    }

    const auto now = core::Utc::now().toRfc3339();
    for (const auto& scene : snapshot.scenes) {
        const auto found = existing.find(scene.id().value());
        if (found == existing.end()) {
            if (auto written = writeScene(scene, now); !written.hasValue()) return written.error();
            continue;
        }
        auto renamed = connection_.prepare(
            "UPDATE scenes SET name=?1 WHERE project_id=?2 AND scene_id=?3");
        if (!renamed.hasValue()) return renamed.error();
        auto renameStatement = std::move(renamed).value();
        if (auto bound = renameStatement.bindText(1, scene.name()); !bound.hasValue()) return bound.error();
        if (auto bound = renameStatement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
        if (auto bound = renameStatement.bindText(3, scene.id().value()); !bound.hasValue()) return bound.error();
        if (auto done = expectDone(renameStatement, "scene rename"); !done.hasValue()) return done.error();
        const auto priorSources = existingSources.find(scene.id().value());
        if (priorSources != existingSources.end() &&
            priorSources->second == scene.sources()) {
            continue;
        }
        auto clearSources = connection_.prepare(
            "DELETE FROM scene_sources WHERE scene_id=?1");
        if (!clearSources.hasValue()) return clearSources.error();
        auto clearSourcesStatement = std::move(clearSources).value();
        if (auto bound = clearSourcesStatement.bindText(1, scene.id().value()); !bound.hasValue()) return bound.error();
        if (auto done = expectDone(clearSourcesStatement, "scene source delete"); !done.hasValue()) return done.error();
        if (auto written = writeSceneSources(scene); !written.hasValue()) return written.error();
    }
    auto stateInsert = connection_.prepare(
        "INSERT INTO studio_state(project_id,active_scene_id) VALUES(?1,?2)");
    if (!stateInsert.hasValue()) return stateInsert.error();
    auto stateStatement = std::move(stateInsert).value();
    if (auto bound = stateStatement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    if (auto bound = stateStatement.bindText(2, snapshot.activeSceneId.value()); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(stateStatement, "studio state insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<void> SqliteStudioStore::ensureSessionBelongsToProject(
    const SessionId& sessionId) {
    auto query = connection_.prepare(
        "SELECT count(*) FROM recording_sessions WHERE session_id=?1 AND project_id=?2");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row || statement.columnInt64(0) != 1) {
        return AppError{ErrorCode::NotFound,
                        "recording session does not belong to studio project"};
    }
    return core::ok();
}

Result<void> SqliteStudioStore::ensureSessionIsRecording(
    const SessionId& sessionId) {
    auto query = connection_.prepare(
        "SELECT state FROM recording_sessions WHERE session_id=?1 AND project_id=?2");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound,
                        "recording session does not belong to studio project"};
    }
    if (statement.columnText(0) != "RECORDING") {
        return AppError{ErrorCode::InvalidState,
                        "studio live data requires a recording session"};
    }
    return core::ok();
}

Result<void> SqliteStudioStore::ensureSessionIsImportable(
    const SessionId& sessionId) {
    auto query = connection_.prepare(
        "SELECT state FROM recording_sessions WHERE session_id=?1 AND project_id=?2");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound,
                        "recording session does not belong to studio project"};
    }
    const auto state = statement.columnText(0);
    if (state != "COMPLETED" && state != "RECOVERED") {
        return AppError{ErrorCode::InvalidState,
                        "recording import requires a completed session"};
    }
    return core::ok();
}

Result<void> SqliteStudioStore::prepareRecording(
    const SessionId& sessionId, const std::vector<RecordingSourceRole>& sources,
    const SceneId& activeSceneId) {
    std::unordered_set<std::string> ids;
    std::unordered_set<int> roles;
    for (const auto& source : sources) {
        if (!ids.insert(source.sourceId.value()).second ||
            !roles.insert(static_cast<int>(source.role)).second) {
            return AppError{ErrorCode::InvalidArgument,
                            "recording source identities and roles must be unique"};
        }
    }
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    if (auto valid = ensureSessionIsRecording(sessionId); !valid.hasValue()) return valid.error();
    for (const auto& source : sources) {
        auto inserted = connection_.prepare(
            "INSERT INTO recording_sources(session_id,source_id,role) VALUES(?1,?2,?3)");
        if (!inserted.hasValue()) return inserted.error();
        auto statement = std::move(inserted).value();
        if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
        if (auto bound = statement.bindText(2, source.sourceId.value()); !bound.hasValue()) return bound.error();
        if (auto bound = statement.bindText(3, domain::studioSourceRoleName(source.role)); !bound.hasValue()) return bound.error();
        if (auto done = expectDone(statement, "recording source insert"); !done.hasValue()) return done.error();
    }
    auto event = connection_.prepare(
        "INSERT INTO recording_scene_events(session_id,sequence,scene_id,position_ns) "
        "VALUES(?1,0,?2,0)");
    if (!event.hasValue()) return event.error();
    auto eventStatement = std::move(event).value();
    if (auto bound = eventStatement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = eventStatement.bindText(2, activeSceneId.value()); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(eventStatement, "initial scene event insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<void> SqliteStudioStore::discardRecordingPreparation(
    const SessionId& sessionId) {
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto stateQuery = connection_.prepare(
        "SELECT state FROM recording_sessions WHERE session_id=?1 AND project_id=?2");
    if (!stateQuery.hasValue()) return stateQuery.error();
    auto stateStatement = std::move(stateQuery).value();
    if (auto bound = stateStatement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = stateStatement.bindText(2, projectId_.value()); !bound.hasValue()) return bound.error();
    auto row = stateStatement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound,
                        "recording session does not belong to studio project"};
    }
    if (stateStatement.columnText(0) != "ABORTED") {
        return AppError{ErrorCode::InvalidState,
                        "only aborted recording preparation can be discarded"};
    }
    for (const std::string_view table : {
             "recording_markers", "recording_scene_events", "recording_sources"}) {
        auto removed = connection_.prepare(
            "DELETE FROM " + std::string{table} + " WHERE session_id=?1");
        if (!removed.hasValue()) return removed.error();
        auto statement = std::move(removed).value();
        if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
        if (auto done = expectDone(statement, "recording preparation delete"); !done.hasValue()) return done.error();
    }
    return transaction.commit();
}

Result<void> SqliteStudioStore::recordSceneSwitch(
    const SessionId& sessionId, const SceneId& sceneId,
    std::uint64_t sequence, TimestampNs position) {
    if (position.time_since_epoch() < DurationNs::zero()) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording scene position must not be negative"};
    }
    auto sqlSequence = sqlInteger(sequence, "recording scene sequence");
    if (!sqlSequence.hasValue()) return sqlSequence.error();
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    if (auto valid = ensureSessionIsRecording(sessionId); !valid.hasValue()) return valid.error();
    auto inserted = connection_.prepare(
        "INSERT INTO recording_scene_events(session_id,sequence,scene_id,position_ns) "
        "VALUES(?1,?2,?3,?4)");
    if (!inserted.hasValue()) return inserted.error();
    auto statement = std::move(inserted).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindInt64(2, sqlSequence.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(3, sceneId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindInt64(4, position.time_since_epoch().count()); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(statement, "recording scene event insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<void> SqliteStudioStore::recordMarker(const RecordingMarker& marker) {
    if (marker.markerId.empty() || marker.position.time_since_epoch() < DurationNs::zero()) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording marker is outside valid bounds"};
    }
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    if (auto valid = ensureSessionIsRecording(marker.sessionId); !valid.hasValue()) return valid.error();
    auto inserted = connection_.prepare(
        "INSERT INTO recording_markers(marker_id,session_id,position_ns,label) "
        "VALUES(?1,?2,?3,?4)");
    if (!inserted.hasValue()) return inserted.error();
    auto statement = std::move(inserted).value();
    if (auto bound = statement.bindText(1, marker.markerId); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, marker.sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindInt64(3, marker.position.time_since_epoch().count()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(4, marker.label); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(statement, "recording marker insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<std::vector<RecordingSourceRole>> SqliteStudioStore::loadRecordingSources(
    const SessionId& sessionId) {
    if (auto valid = ensureSessionBelongsToProject(sessionId); !valid.hasValue()) return valid.error();
    auto query = connection_.prepare(
        "SELECT source_id,role FROM recording_sources WHERE session_id=?1 ORDER BY role,source_id");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    std::vector<RecordingSourceRole> sources;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto sourceId = SourceId::create(statement.columnText(0));
        auto role = domain::studioSourceRoleFromName(statement.columnText(1));
        if (!sourceId.hasValue() || !role.hasValue()) return corrupt("recording source is invalid");
        sources.push_back({.sourceId = std::move(sourceId).value(), .role = role.value()});
    }
    return sources;
}

Result<std::vector<RecordingSceneEvent>> SqliteStudioStore::loadRecordingSceneEvents(
    const SessionId& sessionId) {
    if (auto valid = ensureSessionBelongsToProject(sessionId); !valid.hasValue()) return valid.error();
    auto query = connection_.prepare(
        "SELECT sequence,scene_id,position_ns FROM recording_scene_events "
        "WHERE session_id=?1 ORDER BY sequence");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    std::vector<RecordingSceneEvent> events;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        const auto sequence = statement.columnInt64(0);
        const auto position = statement.columnInt64(2);
        auto sceneId = SceneId::create(statement.columnText(1));
        if (sequence < 0 || position < 0 || !sceneId.hasValue()) return corrupt("recording scene event is invalid");
        events.push_back({.sessionId = sessionId,
                          .sequence = static_cast<std::uint64_t>(sequence),
                          .sceneId = std::move(sceneId).value(),
                          .position = TimestampNs{DurationNs{position}}});
    }
    return events;
}

Result<std::vector<RecordingMarker>> SqliteStudioStore::loadRecordingMarkers(
    const SessionId& sessionId) {
    if (auto valid = ensureSessionBelongsToProject(sessionId); !valid.hasValue()) return valid.error();
    auto query = connection_.prepare(
        "SELECT marker_id,position_ns,label FROM recording_markers "
        "WHERE session_id=?1 ORDER BY position_ns,marker_id");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    std::vector<RecordingMarker> markers;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        const auto position = statement.columnInt64(1);
        if (position < 0 || statement.columnText(0).empty()) return corrupt("recording marker is invalid");
        markers.push_back({.markerId = statement.columnText(0),
                           .sessionId = sessionId,
                           .position = TimestampNs{DurationNs{position}},
                           .label = statement.columnText(2)});
    }
    return markers;
}

Result<std::vector<UnimportedRecording>>
SqliteStudioStore::completedUnimportedRecordings() {
    auto query = connection_.prepare(
        "SELECT r.session_id,r.started_ns,r.stopped_ns FROM recording_sessions r "
        "LEFT JOIN recording_imports i ON i.session_id=r.session_id "
        "WHERE r.project_id=?1 AND r.state IN ('COMPLETED','RECOVERED') "
        "AND i.session_id IS NULL ORDER BY r.started_ns,r.session_id");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, projectId_.value()); !bound.hasValue()) return bound.error();
    std::vector<UnimportedRecording> recordings;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto sessionId = SessionId::create(statement.columnText(0));
        const auto started = statement.columnInt64(1);
        const auto stopped = statement.columnInt64(2);
        if (!sessionId.hasValue() || started < 0 || stopped < started) return corrupt("completed recording is invalid");
        recordings.push_back({.sessionId = std::move(sessionId).value(),
                              .startedAt = TimestampNs{DurationNs{started}},
                              .stoppedAt = TimestampNs{DurationNs{stopped}}});
    }
    return recordings;
}

Result<void> SqliteStudioStore::putRecordingImport(
    const RecordingImportRecord& record) {
    if (record.base.time_since_epoch() < DurationNs::zero() ||
        record.importedRevision < 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording import is outside valid bounds"};
    }
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    if (auto valid = ensureSessionIsImportable(record.sessionId); !valid.hasValue()) return valid.error();
    auto existing = recordingImport(record.sessionId);
    if (!existing.hasValue()) return existing.error();
    if (existing.value().has_value()) {
        if (*existing.value() == record) return transaction.commit();
        return AppError{ErrorCode::InvalidState,
                        "recording already has a different import checkpoint"};
    }
    auto inserted = connection_.prepare(
        "INSERT INTO recording_imports(session_id,timeline_id,base_ns,"
        "imported_revision,imported_at_utc) VALUES(?1,?2,?3,?4,?5)");
    if (!inserted.hasValue()) return inserted.error();
    auto statement = std::move(inserted).value();
    if (auto bound = statement.bindText(1, record.sessionId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(2, record.timelineId.value()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindInt64(3, record.base.time_since_epoch().count()); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindInt64(4, record.importedRevision); !bound.hasValue()) return bound.error();
    if (auto bound = statement.bindText(5, record.importedAt.toRfc3339()); !bound.hasValue()) return bound.error();
    if (auto done = expectDone(statement, "recording import insert"); !done.hasValue()) return done.error();
    return transaction.commit();
}

Result<std::optional<RecordingImportRecord>> SqliteStudioStore::recordingImport(
    const SessionId& sessionId) {
    if (auto valid = ensureSessionBelongsToProject(sessionId); !valid.hasValue()) return valid.error();
    auto query = connection_.prepare(
        "SELECT timeline_id,base_ns,imported_revision,imported_at_utc "
        "FROM recording_imports WHERE session_id=?1");
    if (!query.hasValue()) return query.error();
    auto statement = std::move(query).value();
    if (auto bound = statement.bindText(1, sessionId.value()); !bound.hasValue()) return bound.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) return std::optional<RecordingImportRecord>{};
    auto timelineId = TimelineId::create(statement.columnText(0));
    auto importedAt = core::Utc::parseRfc3339(statement.columnText(3));
    const auto base = statement.columnInt64(1);
    const auto revision = statement.columnInt64(2);
    if (!timelineId.hasValue() || !importedAt.hasValue() || base < 0 || revision < 0) return corrupt("recording import is invalid");
    return std::optional<RecordingImportRecord>{RecordingImportRecord{
        .sessionId = sessionId,
        .timelineId = std::move(timelineId).value(),
        .base = TimestampNs{DurationNs{base}},
        .importedRevision = revision,
        .importedAt = std::move(importedAt).value()}};
}

}  // namespace creator::project_store
