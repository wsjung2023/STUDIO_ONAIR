#include "project_store/SqliteTimelineStore.h"

#include "core/AppError.h"
#include "project_store/MigrationRunner.h"
#include "project_store/internal/EditCommandCodec.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::project_store {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::AssetAvailability;
using domain::AssetId;
using domain::AudioAssetMetadata;
using domain::AudioEnvelope;
using domain::Clip;
using domain::ClipKind;
using domain::ClipId;
using domain::CommandId;
using domain::MediaAsset;
using domain::MediaKind;
using domain::ProjectId;
using domain::TimeRange;
using domain::Timeline;
using domain::TimelineId;
using domain::Track;
using domain::TrackId;
using domain::TrackKind;
using domain::VideoAssetMetadata;
using domain::VisualTransform;
using internal::SqliteConnection;
using internal::SqliteStatement;
using internal::SqliteStep;
using internal::SqliteTransaction;

AppError corrupt(std::string message) {
    return AppError{ErrorCode::IoFailure, "sqlite timeline data is invalid: " +
                                              std::move(message)};
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

Result<std::int64_t> unsignedToSqlInteger(std::uint64_t value,
                                         std::string_view field) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return AppError{ErrorCode::InvalidArgument,
                        std::string{field} + " exceeds SQLite int64"};
    }
    return static_cast<std::int64_t>(value);
}

Result<std::size_t> toSize(std::int64_t value, std::string_view field) {
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max())) {
        return corrupt(std::string{field} + " is outside size_t");
    }
    return static_cast<std::size_t>(value);
}

Result<bool> parseBool(std::int64_t value, std::string_view field) {
    if (value == 0) return false;
    if (value == 1) return true;
    return corrupt(std::string{field} + " is not boolean");
}

std::string_view mediaKindText(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::Video: return "VIDEO";
        case MediaKind::Audio: return "AUDIO";
        case MediaKind::Image: return "IMAGE";
    }
    return "";
}

Result<MediaKind> parseMediaKind(std::string_view value) {
    if (value == "VIDEO") return MediaKind::Video;
    if (value == "AUDIO") return MediaKind::Audio;
    if (value == "IMAGE") return MediaKind::Image;
    return corrupt("unknown media kind");
}

std::string_view availabilityText(AssetAvailability availability) noexcept {
    return availability == AssetAvailability::Available ? "AVAILABLE" : "OFFLINE";
}

Result<AssetAvailability> parseAvailability(std::string_view value) {
    if (value == "AVAILABLE") return AssetAvailability::Available;
    if (value == "OFFLINE") return AssetAvailability::Offline;
    return corrupt("unknown asset availability");
}

std::string_view trackKindText(TrackKind kind) noexcept {
    switch (kind) {
        case TrackKind::Video: return "VIDEO";
        case TrackKind::Audio: return "AUDIO";
        case TrackKind::Title: return "TITLE";
        case TrackKind::Caption: return "CAPTION";
    }
    return "";
}

Result<TrackKind> parseTrackKind(std::string_view value) {
    if (value == "VIDEO") return TrackKind::Video;
    if (value == "AUDIO") return TrackKind::Audio;
    if (value == "TITLE") return TrackKind::Title;
    if (value == "CAPTION") return TrackKind::Caption;
    return corrupt("unknown track kind");
}

std::string_view eventKindText(EditEventKind kind) noexcept {
    switch (kind) {
        case EditEventKind::Apply: return "APPLY";
        case EditEventKind::Undo: return "UNDO";
        case EditEventKind::Redo: return "REDO";
    }
    return "";
}

Result<EditEventKind> parseEventKind(std::string_view value) {
    if (value == "APPLY") return EditEventKind::Apply;
    if (value == "UNDO") return EditEventKind::Undo;
    if (value == "REDO") return EditEventKind::Redo;
    return corrupt("unknown edit event kind");
}

Result<MediaAsset> readAssetRow(SqliteStatement& statement) {
    auto id = AssetId::create(statement.columnText(0));
    if (!id.hasValue()) return corrupt("asset id is empty");
    auto kind = parseMediaKind(statement.columnText(1));
    if (!kind.hasValue()) return kind.error();
    const auto duration = statement.columnInt64(3);
    if (duration <= 0) return corrupt("asset duration is not positive");

    std::optional<VideoAssetMetadata> video;
    const bool hasVideo = !statement.columnIsNull(4) || !statement.columnIsNull(5) ||
                          !statement.columnIsNull(6) || !statement.columnIsNull(7);
    if (hasVideo) {
        if (statement.columnIsNull(4) || statement.columnIsNull(5) ||
            statement.columnIsNull(6) || statement.columnIsNull(7)) {
            return corrupt("asset has partial video metadata");
        }
        auto frameRate = core::FrameRate::create(statement.columnInt64(6),
                                                  statement.columnInt64(7));
        if (!frameRate.hasValue()) return corrupt("asset frame rate is invalid");
        const auto width = statement.columnInt64(4);
        const auto height = statement.columnInt64(5);
        if (width < std::numeric_limits<std::int32_t>::min() ||
            width > std::numeric_limits<std::int32_t>::max() ||
            height < std::numeric_limits<std::int32_t>::min() ||
            height > std::numeric_limits<std::int32_t>::max()) {
            return corrupt("asset dimensions are outside int32");
        }
        video = VideoAssetMetadata{.width = static_cast<std::int32_t>(width),
                                   .height = static_cast<std::int32_t>(height),
                                   .frameRate = frameRate.value()};
    }

    std::optional<AudioAssetMetadata> audio;
    const bool hasAudio = !statement.columnIsNull(8) || !statement.columnIsNull(9);
    if (hasAudio) {
        if (statement.columnIsNull(8) || statement.columnIsNull(9)) {
            return corrupt("asset has partial audio metadata");
        }
        const auto sampleRate = statement.columnInt64(8);
        const auto channels = statement.columnInt64(9);
        if (sampleRate < std::numeric_limits<std::int32_t>::min() ||
            sampleRate > std::numeric_limits<std::int32_t>::max() ||
            channels < std::numeric_limits<std::int32_t>::min() ||
            channels > std::numeric_limits<std::int32_t>::max()) {
            return corrupt("asset audio metadata is outside int32");
        }
        audio = AudioAssetMetadata{.sampleRate = static_cast<std::int32_t>(sampleRate),
                                   .channels = static_cast<std::int32_t>(channels)};
    }
    const auto fileSize = statement.columnInt64(10);
    if (fileSize <= 0) return corrupt("asset file size is not positive");
    auto availability = parseAvailability(statement.columnText(12));
    if (!availability.hasValue()) return availability.error();
    auto created = MediaAsset::create(
        std::move(id).value(), kind.value(), statement.columnText(2),
        DurationNs{duration}, std::move(video), std::move(audio),
        static_cast<std::uint64_t>(fileSize), statement.columnText(11),
        availability.value());
    if (!created.hasValue()) return corrupt(created.error().message());
    return std::move(created).value();
}

Result<std::optional<VisualTransform>> loadVisual(
    SqliteConnection& connection, const ClipId& clipId) {
    auto prepared = connection.prepare(
        "SELECT x,y,width,height,scale_x,scale_y,rotation_degrees,"
        "crop_left,crop_top,crop_right,crop_bottom,opacity,z_order "
        "FROM clip_visual_transforms WHERE clip_id=?1");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, clipId.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) return std::optional<VisualTransform>{};
    const auto zOrder = statement.columnInt64(12);
    if (zOrder < std::numeric_limits<std::int32_t>::min() ||
        zOrder > std::numeric_limits<std::int32_t>::max()) {
        return corrupt("visual z-order is outside int32");
    }
    auto value = VisualTransform::create(
        statement.columnDouble(0), statement.columnDouble(1),
        statement.columnDouble(2), statement.columnDouble(3),
        statement.columnDouble(4), statement.columnDouble(5),
        statement.columnDouble(6), statement.columnDouble(7),
        statement.columnDouble(8), statement.columnDouble(9),
        statement.columnDouble(10), statement.columnDouble(11),
        static_cast<std::int32_t>(zOrder));
    if (!value.hasValue()) return corrupt(value.error().message());
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate visual transform");
    return std::optional<VisualTransform>{std::move(value).value()};
}

Result<std::optional<AudioEnvelope>> loadAudioEnvelope(
    SqliteConnection& connection, const ClipId& clipId,
    DurationNs clipDuration) {
    auto prepared = connection.prepare(
        "SELECT gain_db,fade_in_ns,fade_out_ns,clip_duration_ns "
        "FROM clip_audio_envelopes WHERE clip_id=?1");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto bound = statement.bindText(1, clipId.value()); !bound.hasValue()) {
        return bound.error();
    }
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) return std::optional<AudioEnvelope>{};
    if (statement.columnInt64(3) != clipDuration.count()) {
        return corrupt("audio envelope duration differs from clip");
    }
    auto value = AudioEnvelope::create(
        statement.columnDouble(0), DurationNs{statement.columnInt64(1)},
        DurationNs{statement.columnInt64(2)}, clipDuration);
    if (!value.hasValue()) return corrupt(value.error().message());
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate audio envelope");
    return std::optional<AudioEnvelope>{std::move(value).value()};
}

Result<void> bindNullableInt64(SqliteStatement& statement, int index,
                               std::optional<std::int64_t> value) {
    return value.has_value() ? statement.bindInt64(index, *value)
                             : statement.bindNull(index);
}

}  // namespace

Result<SqliteTimelineStore> SqliteTimelineStore::open(
    const std::filesystem::path& databasePath,
    const ProjectId& expectedProjectId) {
    auto opened = SqliteConnection::open(databasePath);
    if (!opened.hasValue()) return opened.error();
    auto connection = std::move(opened).value();
    if (auto migrated = MigrationRunner::apply(connection); !migrated.hasValue()) {
        return migrated.error();
    }
    auto prepared = connection.prepare("SELECT project_id FROM projects");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) return corrupt("project identity is missing");
    const std::string actual = statement.columnText(0);
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("project identity is ambiguous");
    if (actual != expectedProjectId.value()) {
        return AppError{ErrorCode::InvalidArgument,
                        "timeline database project identity does not match package"};
    }
    return SqliteTimelineStore{std::move(connection), expectedProjectId};
}

Result<void> SqliteTimelineStore::putAsset(const MediaAsset& mediaAsset) {
    auto fileSize = unsignedToSqlInteger(mediaAsset.fileSize(), "asset file size");
    if (!fileSize.hasValue()) return fileSize.error();
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();

    auto prepared = connection_.prepare(
        "INSERT OR IGNORE INTO media_assets(asset_id,project_id,kind,relative_path,duration_ns,"
        "width,height,frame_rate_numerator,frame_rate_denominator,sample_rate,channels,"
        "file_size,fingerprint,availability) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto r = statement.bindText(1, mediaAsset.id().value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(2, projectId_.value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(3, mediaKindText(mediaAsset.kind())); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(4, mediaAsset.relativePath()); !r.hasValue()) return r.error();
    if (auto r = statement.bindInt64(5, mediaAsset.duration().count()); !r.hasValue()) return r.error();
    if (mediaAsset.video().has_value()) {
        if (auto r = statement.bindInt64(6, mediaAsset.video()->width); !r.hasValue()) return r.error();
        if (auto r = statement.bindInt64(7, mediaAsset.video()->height); !r.hasValue()) return r.error();
        if (auto r = statement.bindInt64(8, mediaAsset.video()->frameRate.numerator()); !r.hasValue()) return r.error();
        if (auto r = statement.bindInt64(9, mediaAsset.video()->frameRate.denominator()); !r.hasValue()) return r.error();
    } else {
        if (auto r = statement.bindNull(6); !r.hasValue()) return r.error();
        if (auto r = statement.bindNull(7); !r.hasValue()) return r.error();
        if (auto r = statement.bindNull(8); !r.hasValue()) return r.error();
        if (auto r = statement.bindNull(9); !r.hasValue()) return r.error();
    }
    if (mediaAsset.audio().has_value()) {
        if (auto r = statement.bindInt64(10, mediaAsset.audio()->sampleRate); !r.hasValue()) return r.error();
        if (auto r = statement.bindInt64(11, mediaAsset.audio()->channels); !r.hasValue()) return r.error();
    } else {
        if (auto r = statement.bindNull(10); !r.hasValue()) return r.error();
        if (auto r = statement.bindNull(11); !r.hasValue()) return r.error();
    }
    if (auto r = statement.bindInt64(12, fileSize.value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(13, mediaAsset.fingerprint()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(14, availabilityText(mediaAsset.availability())); !r.hasValue()) return r.error();
    if (auto inserted = expectDone(statement, "insert media asset");
        !inserted.hasValue()) {
        return inserted.error();
    }
    if (connection_.changes() == 0) {
        auto existing = asset(mediaAsset.id());
        if (existing.hasValue()) {
            if (existing.value() == mediaAsset) return transaction.commit();
            return AppError{
                ErrorCode::AlreadyExists,
                "media asset identity already has different metadata"};
        }
        if (existing.error().code() != ErrorCode::NotFound) {
            return existing.error();
        }
        auto selectedPath = connection_.prepare(
            "SELECT asset_id FROM media_assets "
            "WHERE project_id=?1 AND relative_path=?2");
        if (!selectedPath.hasValue()) return selectedPath.error();
        auto pathStatement = std::move(selectedPath).value();
        if (auto r = pathStatement.bindText(1, projectId_.value());
            !r.hasValue()) {
            return r.error();
        }
        if (auto r = pathStatement.bindText(2, mediaAsset.relativePath());
            !r.hasValue()) {
            return r.error();
        }
        auto pathRow = pathStatement.step();
        if (!pathRow.hasValue()) return pathRow.error();
        if (pathRow.value() == SqliteStep::Row) {
            return AppError{
                ErrorCode::AlreadyExists,
                "media asset package path already belongs to another identity"};
        }
        return corrupt("media asset insert was ignored without an identity owner");
    }
    return transaction.commit();
}

Result<MediaAsset> SqliteTimelineStore::asset(const AssetId& assetId) {
    auto prepared = connection_.prepare(
        "SELECT asset_id,kind,relative_path,duration_ns,width,height,"
        "frame_rate_numerator,frame_rate_denominator,sample_rate,channels,"
        "file_size,fingerprint,availability FROM media_assets "
        "WHERE asset_id=?1 AND project_id=?2");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto r = statement.bindText(1, assetId.value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(2, projectId_.value()); !r.hasValue()) return r.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) {
        return AppError{ErrorCode::NotFound, "media asset was not found"};
    }
    auto value = readAssetRow(statement);
    if (!value.hasValue()) return value.error();
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate asset identity");
    return std::move(value).value();
}

Result<std::vector<MediaAsset>> SqliteTimelineStore::assets() {
    auto prepared = connection_.prepare(
        "SELECT asset_id,kind,relative_path,duration_ns,width,height,"
        "frame_rate_numerator,frame_rate_denominator,sample_rate,channels,"
        "file_size,fingerprint,availability FROM media_assets "
        "WHERE project_id=?1 ORDER BY asset_id");
    if (!prepared.hasValue()) return prepared.error();
    auto statement = std::move(prepared).value();
    if (auto r = statement.bindText(1, projectId_.value()); !r.hasValue()) return r.error();
    std::vector<MediaAsset> values;
    while (true) {
        auto row = statement.step();
        if (!row.hasValue()) return row.error();
        if (row.value() == SqliteStep::Done) break;
        auto value = readAssetRow(statement);
        if (!value.hasValue()) return value.error();
        values.push_back(std::move(value).value());
    }
    return values;
}

Result<void> SqliteTimelineStore::writeSnapshot(const Timeline& timeline) {
    auto deleted = connection_.prepare("DELETE FROM tracks WHERE timeline_id=?1");
    if (!deleted.hasValue()) return deleted.error();
    auto deleteStatement = std::move(deleted).value();
    if (auto r = deleteStatement.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    if (auto r = expectDone(deleteStatement, "replace timeline tracks"); !r.hasValue()) return r.error();

    std::size_t position = 0;
    for (const auto& track : timeline.tracks()) {
        auto sqlPosition = unsignedToSqlInteger(position++, "track position");
        if (!sqlPosition.hasValue()) return sqlPosition.error();
        auto insertedTrack = connection_.prepare(
            "INSERT INTO tracks(track_id,timeline_id,kind,name,position,enabled,locked) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7)");
        if (!insertedTrack.hasValue()) return insertedTrack.error();
        auto trackStatement = std::move(insertedTrack).value();
        if (auto r = trackStatement.bindText(1, track.id().value()); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindText(2, timeline.id().value()); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindText(3, trackKindText(track.kind())); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindText(4, track.name()); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindInt64(5, sqlPosition.value()); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindInt64(6, track.enabled() ? 1 : 0); !r.hasValue()) return r.error();
        if (auto r = trackStatement.bindInt64(7, track.locked() ? 1 : 0); !r.hasValue()) return r.error();
        if (auto r = expectDone(trackStatement, "insert timeline track"); !r.hasValue()) return r.error();

        for (const auto& clip : track.clips()) {
            if (clip.kind() != ClipKind::Asset || !clip.assetId().has_value()) {
                return AppError{ErrorCode::UnsupportedVersion,
                                "generated clips are not available in R1-01"};
            }
            auto insertedClip = connection_.prepare(
                "INSERT INTO clips(clip_id,track_id,clip_kind,asset_id,media_kind,"
                "source_start_ns,source_duration_ns,timeline_start_ns,"
                "timeline_duration_ns,enabled) VALUES(?1,?2,'ASSET',?3,?4,?5,?6,?7,?8,?9)");
            if (!insertedClip.hasValue()) return insertedClip.error();
            auto clipStatement = std::move(insertedClip).value();
            if (auto r = clipStatement.bindText(1, clip.id().value()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindText(2, track.id().value()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindText(3, clip.assetId()->value()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindText(4, mediaKindText(clip.mediaKind())); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindInt64(5, clip.sourceRange().start().time_since_epoch().count()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindInt64(6, clip.sourceRange().duration().count()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindInt64(7, clip.timelineRange().start().time_since_epoch().count()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindInt64(8, clip.timelineRange().duration().count()); !r.hasValue()) return r.error();
            if (auto r = clipStatement.bindInt64(9, clip.enabled() ? 1 : 0); !r.hasValue()) return r.error();
            if (auto r = expectDone(clipStatement, "insert timeline clip"); !r.hasValue()) return r.error();

            if (clip.visualTransform().has_value()) {
                const auto& v = *clip.visualTransform();
                auto inserted = connection_.prepare(
                    "INSERT INTO clip_visual_transforms(clip_id,x,y,width,height,"
                    "scale_x,scale_y,rotation_degrees,crop_left,crop_top,crop_right,"
                    "crop_bottom,opacity,z_order) VALUES("
                    "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
                if (!inserted.hasValue()) return inserted.error();
                auto statement = std::move(inserted).value();
                if (auto r = statement.bindText(1, clip.id().value()); !r.hasValue()) return r.error();
                const double values[]{v.x(), v.y(), v.width(), v.height(), v.scaleX(),
                                      v.scaleY(), v.rotationDegrees(), v.cropLeft(),
                                      v.cropTop(), v.cropRight(), v.cropBottom(), v.opacity()};
                for (int index = 0; index < 12; ++index) {
                    if (auto r = statement.bindDouble(index + 2, values[index]); !r.hasValue()) return r.error();
                }
                if (auto r = statement.bindInt64(14, v.zOrder()); !r.hasValue()) return r.error();
                if (auto r = expectDone(statement, "insert visual transform"); !r.hasValue()) return r.error();
            }
            if (clip.audioEnvelope().has_value()) {
                const auto& a = *clip.audioEnvelope();
                auto inserted = connection_.prepare(
                    "INSERT INTO clip_audio_envelopes(clip_id,gain_db,fade_in_ns,"
                    "fade_out_ns,clip_duration_ns) VALUES(?1,?2,?3,?4,?5)");
                if (!inserted.hasValue()) return inserted.error();
                auto statement = std::move(inserted).value();
                if (auto r = statement.bindText(1, clip.id().value()); !r.hasValue()) return r.error();
                if (auto r = statement.bindDouble(2, a.gainDb()); !r.hasValue()) return r.error();
                if (auto r = statement.bindInt64(3, a.fadeIn().count()); !r.hasValue()) return r.error();
                if (auto r = statement.bindInt64(4, a.fadeOut().count()); !r.hasValue()) return r.error();
                if (auto r = statement.bindInt64(5, clip.timelineRange().duration().count()); !r.hasValue()) return r.error();
                if (auto r = expectDone(statement, "insert audio envelope"); !r.hasValue()) return r.error();
            }
        }
    }
    return core::ok();
}

Result<void> SqliteTimelineStore::createTimeline(const Timeline& timeline) {
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto inserted = connection_.prepare(
        "INSERT INTO timelines(timeline_id,project_id,name,frame_rate_numerator,"
        "frame_rate_denominator,revision,is_primary) VALUES(?1,?2,?3,?4,?5,0,1)");
    if (!inserted.hasValue()) return inserted.error();
    auto statement = std::move(inserted).value();
    if (auto r = statement.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(2, projectId_.value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(3, timeline.name()); !r.hasValue()) return r.error();
    if (auto r = statement.bindInt64(4, timeline.frameRate().numerator()); !r.hasValue()) return r.error();
    if (auto r = statement.bindInt64(5, timeline.frameRate().denominator()); !r.hasValue()) return r.error();
    if (auto r = expectDone(statement, "insert timeline"); !r.hasValue()) return r.error();
    auto checkpoint = connection_.prepare(
        "INSERT INTO edit_checkpoints(timeline_id,revision,history_count,"
        "history_cursor,clean_cursor,explicit_saved_revision) "
        "VALUES(?1,0,0,0,0,0)");
    if (!checkpoint.hasValue()) return checkpoint.error();
    auto checkpointStatement = std::move(checkpoint).value();
    if (auto r = checkpointStatement.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    if (auto r = expectDone(checkpointStatement, "insert edit checkpoint"); !r.hasValue()) return r.error();
    if (auto written = writeSnapshot(timeline); !written.hasValue()) return written.error();
    return transaction.commit();
}

Result<PersistedTimeline> SqliteTimelineStore::loadPrimaryTimeline() {
    auto selected = connection_.prepare(
        "SELECT timeline_id,name,frame_rate_numerator,frame_rate_denominator,revision "
        "FROM timelines WHERE project_id=?1 AND is_primary=1");
    if (!selected.hasValue()) return selected.error();
    auto timelineStatement = std::move(selected).value();
    if (auto r = timelineStatement.bindText(1, projectId_.value()); !r.hasValue()) return r.error();
    auto row = timelineStatement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() == SqliteStep::Done) {
        return AppError{ErrorCode::NotFound, "primary timeline was not found"};
    }
    auto timelineId = TimelineId::create(timelineStatement.columnText(0));
    if (!timelineId.hasValue()) return corrupt("timeline id is empty");
    auto frameRate = core::FrameRate::create(timelineStatement.columnInt64(2),
                                              timelineStatement.columnInt64(3));
    if (!frameRate.hasValue()) return corrupt("timeline frame rate is invalid");
    auto timelineResult = Timeline::create(std::move(timelineId).value(),
                                           timelineStatement.columnText(1),
                                           frameRate.value());
    if (!timelineResult.hasValue()) return corrupt(timelineResult.error().message());
    Timeline timeline = std::move(timelineResult).value();
    const std::int64_t revision = timelineStatement.columnInt64(4);
    if (revision < 0) return corrupt("timeline revision is negative");
    auto timelineEnd = timelineStatement.step();
    if (!timelineEnd.hasValue()) return timelineEnd.error();
    if (timelineEnd.value() != SqliteStep::Done) return corrupt("multiple primary timelines");

    auto selectedTracks = connection_.prepare(
        "SELECT track_id,kind,name,enabled,locked FROM tracks "
        "WHERE timeline_id=?1 ORDER BY position");
    if (!selectedTracks.hasValue()) return selectedTracks.error();
    auto trackStatement = std::move(selectedTracks).value();
    if (auto r = trackStatement.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    while (true) {
        auto trackRow = trackStatement.step();
        if (!trackRow.hasValue()) return trackRow.error();
        if (trackRow.value() == SqliteStep::Done) break;
        auto id = TrackId::create(trackStatement.columnText(0));
        if (!id.hasValue()) return corrupt("track id is empty");
        auto kind = parseTrackKind(trackStatement.columnText(1));
        if (!kind.hasValue()) return kind.error();
        auto enabled = parseBool(trackStatement.columnInt64(3), "track enabled");
        if (!enabled.hasValue()) return enabled.error();
        auto locked = parseBool(trackStatement.columnInt64(4), "track locked");
        if (!locked.hasValue()) return locked.error();
        const TrackId persistedId = id.value();
        auto track = Track::create(std::move(id).value(), kind.value(),
                                   trackStatement.columnText(2), enabled.value(), false);
        if (!track.hasValue()) return corrupt(track.error().message());
        if (auto added = timeline.addTrack(std::move(track).value()); !added.hasValue()) {
            return corrupt(added.error().message());
        }

        auto selectedClips = connection_.prepare(
            "SELECT clip_id,clip_kind,asset_id,media_kind,source_start_ns,"
            "source_duration_ns,timeline_start_ns,timeline_duration_ns,enabled "
            "FROM clips WHERE track_id=?1 ORDER BY timeline_start_ns,clip_id");
        if (!selectedClips.hasValue()) return selectedClips.error();
        auto clipStatement = std::move(selectedClips).value();
        if (auto r = clipStatement.bindText(1, persistedId.value()); !r.hasValue()) return r.error();
        while (true) {
            auto clipRow = clipStatement.step();
            if (!clipRow.hasValue()) return clipRow.error();
            if (clipRow.value() == SqliteStep::Done) break;
            if (clipStatement.columnText(1) != "ASSET") {
                return AppError{ErrorCode::UnsupportedVersion,
                                "generated clips are not available in R1-01"};
            }
            auto clipId = ClipId::create(clipStatement.columnText(0));
            auto assetId = AssetId::create(clipStatement.columnText(2));
            if (!clipId.hasValue() || !assetId.hasValue()) return corrupt("clip identity is empty");
            auto mediaKind = parseMediaKind(clipStatement.columnText(3));
            if (!mediaKind.hasValue()) return mediaKind.error();
            auto mediaAsset = asset(assetId.value());
            if (!mediaAsset.hasValue()) return corrupt("clip asset is missing");
            if (mediaAsset.value().kind() != mediaKind.value()) {
                return corrupt("clip media kind differs from asset");
            }
            auto source = TimeRange::create(
                TimestampNs{DurationNs{clipStatement.columnInt64(4)}},
                DurationNs{clipStatement.columnInt64(5)});
            auto placed = TimeRange::create(
                TimestampNs{DurationNs{clipStatement.columnInt64(6)}},
                DurationNs{clipStatement.columnInt64(7)});
            if (!source.hasValue() || !placed.hasValue()) return corrupt("clip range is invalid");
            auto clipEnabled = parseBool(clipStatement.columnInt64(8), "clip enabled");
            if (!clipEnabled.hasValue()) return clipEnabled.error();
            auto visual = loadVisual(connection_, clipId.value());
            if (!visual.hasValue()) return visual.error();
            auto envelope = loadAudioEnvelope(connection_, clipId.value(), placed.value().duration());
            if (!envelope.hasValue()) return envelope.error();
            auto clip = Clip::createAsset(
                std::move(clipId).value(), mediaAsset.value(), source.value(), placed.value(),
                clipEnabled.value(), std::move(visual).value(), std::move(envelope).value());
            if (!clip.hasValue()) return corrupt(clip.error().message());
            if (auto inserted = timeline.insertClip(persistedId, std::move(clip).value());
                !inserted.hasValue()) {
                return corrupt(inserted.error().message());
            }
        }
        if (locked.value()) {
            if (auto set = timeline.setTrackLocked(persistedId, true); !set.hasValue()) {
                return corrupt(set.error().message());
            }
        }
    }

    auto selectedCheckpoint = connection_.prepare(
        "SELECT revision,history_count,history_cursor,clean_cursor,"
        "explicit_saved_revision FROM edit_checkpoints WHERE timeline_id=?1");
    if (!selectedCheckpoint.hasValue()) return selectedCheckpoint.error();
    auto checkpoint = std::move(selectedCheckpoint).value();
    if (auto r = checkpoint.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    auto checkpointRow = checkpoint.step();
    if (!checkpointRow.hasValue()) return checkpointRow.error();
    if (checkpointRow.value() != SqliteStep::Row) return corrupt("edit checkpoint is missing");
    if (checkpoint.columnInt64(0) != revision) return corrupt("checkpoint revision differs");
    auto historyCount = toSize(checkpoint.columnInt64(1), "history count");
    auto historyCursor = toSize(checkpoint.columnInt64(2), "history cursor");
    if (!historyCount.hasValue()) return historyCount.error();
    if (!historyCursor.hasValue()) return historyCursor.error();
    if (historyCursor.value() > historyCount.value()) {
        return corrupt("history cursor exceeds history count");
    }
    std::optional<std::size_t> cleanCursor;
    if (!checkpoint.columnIsNull(3)) {
        auto clean = toSize(checkpoint.columnInt64(3), "clean cursor");
        if (!clean.hasValue()) return clean.error();
        if (clean.value() > historyCount.value()) {
            return corrupt("clean cursor exceeds history count");
        }
        cleanCursor = clean.value();
    }
    const auto explicitSavedRevision = checkpoint.columnInt64(4);
    if (explicitSavedRevision < 0 || explicitSavedRevision > revision) {
        return corrupt("explicit saved revision is outside timeline revision");
    }
    auto checkpointEnd = checkpoint.step();
    if (!checkpointEnd.hasValue()) return checkpointEnd.error();
    if (checkpointEnd.value() != SqliteStep::Done) return corrupt("duplicate edit checkpoint");

    std::vector<EditEventRecord> events;
    auto selectedEvents = connection_.prepare(
        "SELECT sequence,event_id,event_kind,command_id,command_type,payload_json,"
        "undo_payload_json,created_at_utc FROM edit_commands "
        "WHERE timeline_id=?1 ORDER BY sequence");
    if (!selectedEvents.hasValue()) return selectedEvents.error();
    auto eventStatement = std::move(selectedEvents).value();
    if (auto r = eventStatement.bindText(1, timeline.id().value()); !r.hasValue()) return r.error();
    std::int64_t expectedSequence = 0;
    while (true) {
        auto eventRow = eventStatement.step();
        if (!eventRow.hasValue()) return eventRow.error();
        if (eventRow.value() == SqliteStep::Done) break;
        if (eventStatement.columnInt64(0) != expectedSequence) {
            return corrupt("edit event sequence is not contiguous");
        }
        if (expectedSequence == std::numeric_limits<std::int64_t>::max()) {
            return corrupt("edit event sequence exceeds int64");
        }
        ++expectedSequence;
        auto kind = parseEventKind(eventStatement.columnText(2));
        if (!kind.hasValue()) return kind.error();
        auto commandId = CommandId::create(eventStatement.columnText(3));
        if (!commandId.hasValue()) return corrupt("event command id is empty");
        auto createdAt = core::Utc::parseRfc3339(eventStatement.columnText(7));
        if (!createdAt.hasValue()) return corrupt("event UTC timestamp is invalid");
        events.push_back(EditEventRecord{
            .eventId = eventStatement.columnText(1),
            .kind = kind.value(),
            .command = domain::EditCommandRecord{
                .commandId = std::move(commandId).value(),
                .type = eventStatement.columnText(4),
                .payload = eventStatement.columnText(5),
                .undoPayload = eventStatement.columnText(6)},
            .createdAt = createdAt.value()});
    }
    if (expectedSequence != revision) {
        return corrupt("edit event count differs from timeline revision");
    }
    return PersistedTimeline{.timeline = std::move(timeline),
                             .revision = revision,
                             .historyCount = historyCount.value(),
                             .historyCursor = historyCursor.value(),
                             .cleanCursor = cleanCursor,
                             .explicitSavedRevision = explicitSavedRevision,
                             .events = std::move(events)};
}

Result<domain::EditHistory> SqliteTimelineStore::loadEditHistory(
    std::size_t limit) {
    auto session = loadEditSession(limit);
    if (!session.hasValue()) return session.error();
    return std::move(session).value().history;
}

Result<PersistedEditSession> SqliteTimelineStore::loadEditSession(
    std::size_t historyLimit) {
    if (historyLimit == 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "edit history limit must be positive"};
    }
    auto begun = SqliteTransaction::beginDeferred(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto persisted = loadPrimaryTimeline();
    if (!persisted.hasValue()) return persisted.error();
    auto history = decodeHistory(persisted.value(), historyLimit);
    if (!history.hasValue()) return history.error();
    if (auto committed = transaction.commit(); !committed.hasValue()) {
        return committed.error();
    }
    return PersistedEditSession{.persisted = std::move(persisted).value(),
                                .history = std::move(history).value()};
}

Result<domain::EditHistory> SqliteTimelineStore::decodeHistory(
    const PersistedTimeline& persisted, std::size_t limit) {
    if (limit == 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "edit history limit must be positive"};
    }
    if (persisted.historyCount > limit) {
        return corrupt("history count exceeds configured limit");
    }

    std::vector<domain::EditCommandRecord> records;
    std::size_t cursor = 0;
    for (const auto& event : persisted.events) {
        switch (event.kind) {
            case EditEventKind::Apply:
                if (cursor < records.size()) {
                    records.erase(
                        records.begin() + static_cast<std::ptrdiff_t>(cursor),
                        records.end());
                }
                records.push_back(event.command);
                cursor = records.size();
                if (records.size() > limit) {
                    records.erase(records.begin());
                    --cursor;
                }
                break;
            case EditEventKind::Undo:
                if (cursor == 0 ||
                    records[cursor - 1].commandId != event.command.commandId ||
                    records[cursor - 1].type != event.command.type) {
                    return corrupt("undo audit event does not match history");
                }
                --cursor;
                break;
            case EditEventKind::Redo:
                if (cursor >= records.size() ||
                    records[cursor].commandId != event.command.commandId ||
                    records[cursor].type != event.command.type) {
                    return corrupt("redo audit event does not match history");
                }
                ++cursor;
                break;
        }
    }
    if (records.size() != persisted.historyCount ||
        cursor != persisted.historyCursor) {
        return corrupt("audit events differ from edit checkpoint");
    }

    internal::EditCommandCodec codec{
        [this](const AssetId& id) { return asset(id); }};
    std::vector<std::unique_ptr<domain::IEditCommand>> commands;
    commands.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto decoded = codec.decode(records[index], index < cursor);
        if (!decoded.hasValue()) return decoded.error();
        commands.push_back(std::move(decoded).value());
    }
    Timeline replay = persisted.timeline;
    std::vector<std::unique_ptr<domain::IEditCommand>> verification;
    verification.reserve(commands.size());
    for (const auto& command : commands) {
        verification.push_back(command->clone());
    }
    for (std::size_t index = cursor; index > 0; --index) {
        auto undone = verification[index - 1]->undo(replay);
        if (!undone.hasValue()) {
            return corrupt("applied edit cannot undo during recovery verification");
        }
    }
    Timeline reconstructedAtCursor = replay;
    for (std::size_t index = 0; index < verification.size(); ++index) {
        const auto expectedRecord = verification[index]->record();
        auto executed = verification[index]->execute(replay);
        if (!executed.hasValue()) {
            return corrupt("edit cannot execute during recovery verification");
        }
        if (verification[index]->record() != expectedRecord) {
            return corrupt("edit undo state contradicts recovered command sequence");
        }
        if (index + 1 == cursor) reconstructedAtCursor = replay;
    }
    if (reconstructedAtCursor != persisted.timeline) {
        return corrupt("edit history does not reconstruct the persisted timeline");
    }
    return domain::EditHistory::restore(
        limit, std::move(commands), cursor, persisted.cleanCursor);
}

Result<void> SqliteTimelineStore::commitEdit(const TimelineCommit& commit) {
    if (commit.expectedRevision < 0 ||
        commit.expectedRevision == std::numeric_limits<std::int64_t>::max() ||
        commit.historyCursor > commit.historyCount ||
        (commit.cleanCursor.has_value() && *commit.cleanCursor > commit.historyCount) ||
        commit.event.eventId.empty() || commit.event.command.type.empty() ||
        commit.event.command.payload.empty() || commit.event.command.undoPayload.empty()) {
        return AppError{ErrorCode::InvalidArgument, "timeline commit metadata is invalid"};
    }
    auto historyCount = unsignedToSqlInteger(commit.historyCount, "history count");
    auto historyCursor = unsignedToSqlInteger(commit.historyCursor, "history cursor");
    if (!historyCount.hasValue()) return historyCount.error();
    if (!historyCursor.hasValue()) return historyCursor.error();
    std::optional<std::int64_t> cleanCursor;
    if (commit.cleanCursor.has_value()) {
        auto converted = unsignedToSqlInteger(*commit.cleanCursor, "clean cursor");
        if (!converted.hasValue()) return converted.error();
        cleanCursor = converted.value();
    }

    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();
    auto selected = connection_.prepare(
        "SELECT revision FROM timelines WHERE timeline_id=?1 AND project_id=?2");
    if (!selected.hasValue()) return selected.error();
    auto selectedStatement = std::move(selected).value();
    if (auto r = selectedStatement.bindText(1, commit.snapshot.id().value()); !r.hasValue()) return r.error();
    if (auto r = selectedStatement.bindText(2, projectId_.value()); !r.hasValue()) return r.error();
    auto row = selectedStatement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound, "timeline to commit was not found"};
    }
    const auto storedRevision = selectedStatement.columnInt64(0);
    if (storedRevision != commit.expectedRevision) {
        return AppError{ErrorCode::InvalidState, "timeline revision is stale"};
    }
    auto end = selectedStatement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate timeline identity");

    if (auto written = writeSnapshot(commit.snapshot); !written.hasValue()) {
        return written.error();
    }
    auto insertedEvent = connection_.prepare(
        "INSERT INTO edit_commands(event_id,timeline_id,sequence,command_id,event_kind,"
        "command_type,payload_json,undo_payload_json,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    if (!insertedEvent.hasValue()) return insertedEvent.error();
    auto eventStatement = std::move(insertedEvent).value();
    if (auto r = eventStatement.bindText(1, commit.event.eventId); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(2, commit.snapshot.id().value()); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindInt64(3, commit.expectedRevision); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(4, commit.event.command.commandId.value()); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(5, eventKindText(commit.event.kind)); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(6, commit.event.command.type); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(7, commit.event.command.payload); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(8, commit.event.command.undoPayload); !r.hasValue()) return r.error();
    if (auto r = eventStatement.bindText(9, commit.event.createdAt.toRfc3339()); !r.hasValue()) return r.error();
    if (auto r = expectDone(eventStatement, "insert edit event"); !r.hasValue()) return r.error();

    const auto newRevision = commit.expectedRevision + 1;
    auto updatedTimeline = connection_.prepare(
        "UPDATE timelines SET name=?1,frame_rate_numerator=?2,"
        "frame_rate_denominator=?3,revision=?4 WHERE timeline_id=?5");
    if (!updatedTimeline.hasValue()) return updatedTimeline.error();
    auto timelineStatement = std::move(updatedTimeline).value();
    if (auto r = timelineStatement.bindText(1, commit.snapshot.name()); !r.hasValue()) return r.error();
    if (auto r = timelineStatement.bindInt64(2, commit.snapshot.frameRate().numerator()); !r.hasValue()) return r.error();
    if (auto r = timelineStatement.bindInt64(3, commit.snapshot.frameRate().denominator()); !r.hasValue()) return r.error();
    if (auto r = timelineStatement.bindInt64(4, newRevision); !r.hasValue()) return r.error();
    if (auto r = timelineStatement.bindText(5, commit.snapshot.id().value()); !r.hasValue()) return r.error();
    if (auto r = expectDone(timelineStatement, "update timeline revision"); !r.hasValue()) return r.error();

    auto updatedCheckpoint = connection_.prepare(
        "UPDATE edit_checkpoints SET revision=?1,history_count=?2,history_cursor=?3,"
        "clean_cursor=?4 WHERE timeline_id=?5");
    if (!updatedCheckpoint.hasValue()) return updatedCheckpoint.error();
    auto checkpoint = std::move(updatedCheckpoint).value();
    if (auto r = checkpoint.bindInt64(1, newRevision); !r.hasValue()) return r.error();
    if (auto r = checkpoint.bindInt64(2, historyCount.value()); !r.hasValue()) return r.error();
    if (auto r = checkpoint.bindInt64(3, historyCursor.value()); !r.hasValue()) return r.error();
    if (auto r = bindNullableInt64(checkpoint, 4, cleanCursor); !r.hasValue()) return r.error();
    if (auto r = checkpoint.bindText(5, commit.snapshot.id().value()); !r.hasValue()) return r.error();
    if (auto r = expectDone(checkpoint, "update edit checkpoint"); !r.hasValue()) return r.error();
    return transaction.commit();
}

Result<void> SqliteTimelineStore::markExplicitSave(
    const TimelineId& timelineId, std::int64_t expectedRevision,
    std::size_t historyCursor) {
    if (expectedRevision < 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "saved timeline revision must not be negative"};
    }
    auto sqlCursor = unsignedToSqlInteger(historyCursor, "history cursor");
    if (!sqlCursor.hasValue()) return sqlCursor.error();
    auto begun = SqliteTransaction::beginImmediate(connection_);
    if (!begun.hasValue()) return begun.error();
    auto transaction = std::move(begun).value();

    auto selected = connection_.prepare(
        "SELECT edit_checkpoints.revision,edit_checkpoints.history_count "
        "FROM edit_checkpoints JOIN timelines USING(timeline_id) "
        "WHERE edit_checkpoints.timeline_id=?1 AND timelines.project_id=?2");
    if (!selected.hasValue()) return selected.error();
    auto statement = std::move(selected).value();
    if (auto r = statement.bindText(1, timelineId.value()); !r.hasValue()) return r.error();
    if (auto r = statement.bindText(2, projectId_.value()); !r.hasValue()) return r.error();
    auto row = statement.step();
    if (!row.hasValue()) return row.error();
    if (row.value() != SqliteStep::Row) {
        return AppError{ErrorCode::NotFound,
                        "timeline save checkpoint was not found"};
    }
    if (statement.columnInt64(0) != expectedRevision) {
        return AppError{ErrorCode::InvalidState,
                        "timeline revision is stale"};
    }
    if (sqlCursor.value() > statement.columnInt64(1)) {
        return AppError{ErrorCode::InvalidArgument,
                        "saved history cursor exceeds history count"};
    }
    auto end = statement.step();
    if (!end.hasValue()) return end.error();
    if (end.value() != SqliteStep::Done) return corrupt("duplicate save checkpoint");

    auto updated = connection_.prepare(
        "UPDATE edit_checkpoints SET clean_cursor=?1,explicit_saved_revision=?2 "
        "WHERE timeline_id=?3");
    if (!updated.hasValue()) return updated.error();
    auto update = std::move(updated).value();
    if (auto r = update.bindInt64(1, sqlCursor.value()); !r.hasValue()) return r.error();
    if (auto r = update.bindInt64(2, expectedRevision); !r.hasValue()) return r.error();
    if (auto r = update.bindText(3, timelineId.value()); !r.hasValue()) return r.error();
    if (auto r = expectDone(update, "mark explicit save"); !r.hasValue()) return r.error();
    return transaction.commit();
}

}  // namespace creator::project_store
