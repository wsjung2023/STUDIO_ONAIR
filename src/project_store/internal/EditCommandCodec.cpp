#include "project_store/internal/EditCommandCodec.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/DeleteRangeCommand.h"
#include "domain/GeneratedClipCommands.h"
#include "domain/SetAudioEnvelopeCommand.h"
#include "domain/SetVisualTransformCommand.h"
#include "domain/SplitClipCommand.h"
#include "domain/TimelineTypes.h"
#include "domain/TrimClipCommand.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::project_store::internal {
namespace {

using Json = nlohmann::json;
using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;
using domain::AssetId;
using domain::AudioEnvelope;
using domain::CaptionCue;
using domain::Clip;
using domain::ClipId;
using domain::CueId;
using domain::DeleteRangeCommand;
using domain::MediaKind;
using domain::RgbaColor;
using domain::TextAlignment;
using domain::TimeRange;
using domain::TrackId;
using domain::TitlePayload;
using domain::TrimEdge;
using domain::VisualTransform;

AppError parseError(std::string message) {
    return AppError{ErrorCode::ParseFailure,
                    "edit command JSON is invalid: " + std::move(message)};
}

Result<void> exactObject(
    const Json& value, std::initializer_list<std::string_view> keys,
    std::string_view label) {
    if (!value.is_object() || value.size() != keys.size()) {
        return parseError(std::string{label} + " has unexpected fields");
    }
    for (const auto key : keys) {
        if (!value.contains(std::string{key})) {
            return parseError(std::string{label} + " is missing " +
                              std::string{key});
        }
    }
    return core::ok();
}

Result<std::string> textField(const Json& value, std::string_view key) {
    const auto& field = value.at(std::string{key});
    if (!field.is_string()) return parseError(std::string{key} + " is not text");
    return field.get<std::string>();
}

Result<std::int64_t> integerField(const Json& value, std::string_view key) {
    const auto& field = value.at(std::string{key});
    if (field.is_number_unsigned()) {
        const auto number = field.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
            return parseError(std::string{key} + " exceeds int64");
        }
        return static_cast<std::int64_t>(number);
    }
    if (!field.is_number_integer()) {
        return parseError(std::string{key} + " is not an integer");
    }
    return field.get<std::int64_t>();
}

Result<double> numberField(const Json& value, std::string_view key) {
    const auto& field = value.at(std::string{key});
    if (!field.is_number()) return parseError(std::string{key} + " is not numeric");
    return field.get<double>();
}

Result<bool> boolField(const Json& value, std::string_view key) {
    const auto& field = value.at(std::string{key});
    if (!field.is_boolean()) return parseError(std::string{key} + " is not boolean");
    return field.get<bool>();
}

template <typename Id>
Result<Id> idField(const Json& value, std::string_view key) {
    auto text = textField(value, key);
    if (!text.hasValue()) return text.error();
    auto id = Id::create(std::move(text).value());
    if (!id.hasValue()) return parseError(std::string{key} + " is empty");
    return id;
}

Result<MediaKind> mediaKind(const Json& value) {
    auto text = textField(value, "mediaKind");
    if (!text.hasValue()) return text.error();
    if (text.value() == "VIDEO") return MediaKind::Video;
    if (text.value() == "AUDIO") return MediaKind::Audio;
    if (text.value() == "IMAGE") return MediaKind::Image;
    return parseError("mediaKind is unknown");
}

Result<std::optional<VisualTransform>> visual(const Json& value) {
    if (value.is_null()) return std::optional<VisualTransform>{};
    if (auto exact = exactObject(
            value,
            {"cropBottom", "cropLeft", "cropRight", "cropTop", "height",
             "opacity", "rotationDegrees", "scaleX", "scaleY", "width", "x",
             "y", "zOrder"},
            "visual");
        !exact.hasValue()) {
        return exact.error();
    }
    auto z = integerField(value, "zOrder");
    if (!z.hasValue()) return z.error();
    if (z.value() < std::numeric_limits<std::int32_t>::min() ||
        z.value() > std::numeric_limits<std::int32_t>::max()) {
        return parseError("zOrder exceeds int32");
    }
    auto x = numberField(value, "x");
    auto y = numberField(value, "y");
    auto width = numberField(value, "width");
    auto height = numberField(value, "height");
    auto scaleX = numberField(value, "scaleX");
    auto scaleY = numberField(value, "scaleY");
    auto rotation = numberField(value, "rotationDegrees");
    auto cropLeft = numberField(value, "cropLeft");
    auto cropTop = numberField(value, "cropTop");
    auto cropRight = numberField(value, "cropRight");
    auto cropBottom = numberField(value, "cropBottom");
    auto opacity = numberField(value, "opacity");
    if (!x.hasValue()) return x.error();
    if (!y.hasValue()) return y.error();
    if (!width.hasValue()) return width.error();
    if (!height.hasValue()) return height.error();
    if (!scaleX.hasValue()) return scaleX.error();
    if (!scaleY.hasValue()) return scaleY.error();
    if (!rotation.hasValue()) return rotation.error();
    if (!cropLeft.hasValue()) return cropLeft.error();
    if (!cropTop.hasValue()) return cropTop.error();
    if (!cropRight.hasValue()) return cropRight.error();
    if (!cropBottom.hasValue()) return cropBottom.error();
    if (!opacity.hasValue()) return opacity.error();
    auto created = VisualTransform::create(
        x.value(), y.value(), width.value(), height.value(), scaleX.value(),
        scaleY.value(), rotation.value(), cropLeft.value(), cropTop.value(),
        cropRight.value(), cropBottom.value(), opacity.value(),
        static_cast<std::int32_t>(z.value()));
    if (!created.hasValue()) return parseError(created.error().message());
    return std::optional<VisualTransform>{created.value()};
}

Result<std::optional<AudioEnvelope>> audio(
    const Json& value, DurationNs clipDuration) {
    if (value.is_null()) return std::optional<AudioEnvelope>{};
    if (auto exact = exactObject(value, {"fadeInNs", "fadeOutNs", "gainDb"},
                                 "audio");
        !exact.hasValue()) {
        return exact.error();
    }
    auto gain = numberField(value, "gainDb");
    auto fadeIn = integerField(value, "fadeInNs");
    auto fadeOut = integerField(value, "fadeOutNs");
    if (!gain.hasValue()) return gain.error();
    if (!fadeIn.hasValue()) return fadeIn.error();
    if (!fadeOut.hasValue()) return fadeOut.error();
    auto created = AudioEnvelope::create(gain.value(), DurationNs{fadeIn.value()},
                                         DurationNs{fadeOut.value()}, clipDuration);
    if (!created.hasValue()) return parseError(created.error().message());
    return std::optional<AudioEnvelope>{created.value()};
}

Result<void> versionOne(const Json& value) {
    auto version = integerField(value, "version");
    if (!version.hasValue()) return version.error();
    if (version.value() != 1) return parseError("command version is unsupported");
    return core::ok();
}

Result<TitlePayload> titlePayload(const Json& value) {
    if (auto exact = exactObject(
            value, {"alignment", "background", "fontFamily", "foreground",
                    "text", "x", "y"}, "title");
        !exact.hasValue()) return exact.error();
    auto alignmentText = textField(value, "alignment");
    auto backgroundText = textField(value, "background");
    auto family = textField(value, "fontFamily");
    auto foregroundText = textField(value, "foreground");
    auto text = textField(value, "text");
    auto x = numberField(value, "x");
    auto y = numberField(value, "y");
    if (!alignmentText.hasValue()) return alignmentText.error();
    if (!backgroundText.hasValue()) return backgroundText.error();
    if (!family.hasValue()) return family.error();
    if (!foregroundText.hasValue()) return foregroundText.error();
    if (!text.hasValue()) return text.error();
    if (!x.hasValue()) return x.error();
    if (!y.hasValue()) return y.error();
    std::optional<TextAlignment> alignment;
    if (alignmentText.value() == "LEFT") alignment = TextAlignment::Left;
    if (alignmentText.value() == "CENTER") alignment = TextAlignment::Center;
    if (alignmentText.value() == "RIGHT") alignment = TextAlignment::Right;
    if (!alignment.has_value()) return parseError("title alignment is unknown");
    auto foreground = RgbaColor::parse(std::move(foregroundText).value());
    auto background = RgbaColor::parse(std::move(backgroundText).value());
    if (!foreground.hasValue()) return parseError(foreground.error().message());
    if (!background.hasValue()) return parseError(background.error().message());
    auto created = TitlePayload::create(
        std::move(text).value(), std::move(family).value(), x.value(), y.value(),
        foreground.value(), background.value(), *alignment);
    if (!created.hasValue()) return parseError(created.error().message());
    return created;
}

Result<CaptionCue> captionCue(const Json& value) {
    if (auto exact = exactObject(
            value, {"durationNs", "id", "startOffsetNs", "text"}, "caption cue");
        !exact.hasValue()) return exact.error();
    auto id = idField<CueId>(value, "id");
    auto start = integerField(value, "startOffsetNs");
    auto duration = integerField(value, "durationNs");
    auto text = textField(value, "text");
    if (!id.hasValue()) return id.error();
    if (!start.hasValue()) return start.error();
    if (!duration.hasValue()) return duration.error();
    if (!text.hasValue()) return text.error();
    auto created = CaptionCue::create(
        std::move(id).value(), DurationNs{start.value()},
        DurationNs{duration.value()}, std::move(text).value());
    if (!created.hasValue()) return parseError(created.error().message());
    return created;
}

Result<Clip> generatedClip(const Json& value) {
    if (auto exact = exactObject(
            value, {"captionCues", "clipKind", "enabled", "id",
                    "sourceDurationNs", "sourceStartNs", "timelineDurationNs",
                    "timelineStartNs", "title", "visual"}, "generated clip");
        !exact.hasValue()) return exact.error();
    auto id = idField<ClipId>(value, "id");
    auto kind = textField(value, "clipKind");
    auto sourceStart = integerField(value, "sourceStartNs");
    auto sourceDuration = integerField(value, "sourceDurationNs");
    auto timelineStart = integerField(value, "timelineStartNs");
    auto timelineDuration = integerField(value, "timelineDurationNs");
    auto enabled = boolField(value, "enabled");
    auto parsedVisual = visual(value.at("visual"));
    if (!id.hasValue()) return id.error();
    if (!kind.hasValue()) return kind.error();
    if (!sourceStart.hasValue()) return sourceStart.error();
    if (!sourceDuration.hasValue()) return sourceDuration.error();
    if (!timelineStart.hasValue()) return timelineStart.error();
    if (!timelineDuration.hasValue()) return timelineDuration.error();
    if (!enabled.hasValue()) return enabled.error();
    if (!parsedVisual.hasValue()) return parsedVisual.error();
    if (sourceStart.value() != 0 ||
        sourceDuration.value() != timelineDuration.value()) {
        return parseError("generated clip source range is not canonical");
    }
    auto range = TimeRange::create(
        TimestampNs{DurationNs{timelineStart.value()}},
        DurationNs{timelineDuration.value()});
    if (!range.hasValue()) return parseError(range.error().message());
    const auto& cuesJson = value.at("captionCues");
    if (!cuesJson.is_array()) return parseError("captionCues is not an array");
    std::vector<CaptionCue> cues;
    for (const auto& item : cuesJson) {
        auto cue = captionCue(item);
        if (!cue.hasValue()) return cue.error();
        cues.push_back(std::move(cue).value());
    }
    if (kind.value() == "TITLE") {
        if (!cues.empty() || value.at("title").is_null()) {
            return parseError("title clip payload is contradictory");
        }
        auto title = titlePayload(value.at("title"));
        if (!title.hasValue()) return title.error();
        auto created = Clip::createTitle(
            std::move(id).value(), range.value(), enabled.value(),
            std::move(title).value(), std::move(parsedVisual).value());
        if (!created.hasValue()) return parseError(created.error().message());
        return created;
    }
    if (kind.value() == "CAPTION") {
        if (!value.at("title").is_null()) {
            return parseError("caption clip has a title payload");
        }
        auto created = Clip::createCaption(
            std::move(id).value(), range.value(), enabled.value(),
            std::move(cues), std::move(parsedVisual).value());
        if (!created.hasValue()) return parseError(created.error().message());
        return created;
    }
    return parseError("generated clip kind is unknown");
}

Result<Clip> clip(const Json& value,
                  const EditCommandCodec::AssetLoader& assetLoader) {
    if (value.is_object() && value.contains("clipKind")) {
        return generatedClip(value);
    }
    if (auto exact = exactObject(
            value,
            {"assetId", "audio", "enabled", "id", "mediaKind",
             "sourceDurationNs", "sourceStartNs", "timelineDurationNs",
             "timelineStartNs", "visual"},
            "clip");
        !exact.hasValue()) {
        return exact.error();
    }
    auto id = idField<ClipId>(value, "id");
    auto assetId = idField<AssetId>(value, "assetId");
    auto kind = mediaKind(value);
    auto sourceStart = integerField(value, "sourceStartNs");
    auto sourceDuration = integerField(value, "sourceDurationNs");
    auto timelineStart = integerField(value, "timelineStartNs");
    auto timelineDuration = integerField(value, "timelineDurationNs");
    auto enabled = boolField(value, "enabled");
    if (!id.hasValue()) return id.error();
    if (!assetId.hasValue()) return assetId.error();
    if (!kind.hasValue()) return kind.error();
    if (!sourceStart.hasValue()) return sourceStart.error();
    if (!sourceDuration.hasValue()) return sourceDuration.error();
    if (!timelineStart.hasValue()) return timelineStart.error();
    if (!timelineDuration.hasValue()) return timelineDuration.error();
    if (!enabled.hasValue()) return enabled.error();
    auto source = TimeRange::create(TimestampNs{DurationNs{sourceStart.value()}},
                                    DurationNs{sourceDuration.value()});
    auto placed = TimeRange::create(TimestampNs{DurationNs{timelineStart.value()}},
                                    DurationNs{timelineDuration.value()});
    if (!source.hasValue()) return parseError(source.error().message());
    if (!placed.hasValue()) return parseError(placed.error().message());
    auto asset = assetLoader(assetId.value());
    if (!asset.hasValue()) return parseError("clip asset could not be loaded");
    if (asset.value().kind() != kind.value()) {
        return parseError("clip mediaKind differs from asset");
    }
    auto parsedVisual = visual(value.at("visual"));
    auto parsedAudio = audio(value.at("audio"), placed.value().duration());
    if (!parsedVisual.hasValue()) return parsedVisual.error();
    if (!parsedAudio.hasValue()) return parsedAudio.error();
    auto created = Clip::createAsset(
        std::move(id).value(), asset.value(), source.value(), placed.value(),
        enabled.value(), std::move(parsedVisual).value(),
        std::move(parsedAudio).value());
    if (!created.hasValue()) return parseError(created.error().message());
    return created;
}

Result<std::vector<DeleteRangeCommand::PreviousTrack>> previousTracks(
    const Json& value, const EditCommandCodec::AssetLoader& assetLoader) {
    if (auto exact = exactObject(value, {"tracks"}, "delete undo");
        !exact.hasValue()) {
        return exact.error();
    }
    const auto& tracks = value.at("tracks");
    if (!tracks.is_array()) return parseError("tracks is not an array");
    std::vector<DeleteRangeCommand::PreviousTrack> result;
    result.reserve(tracks.size());
    std::set<std::string> trackIds;
    std::set<std::string> clipIds;
    for (const auto& track : tracks) {
        if (auto exact = exactObject(track, {"clips", "trackId"}, "track undo");
            !exact.hasValue()) {
            return exact.error();
        }
        auto trackId = idField<TrackId>(track, "trackId");
        if (!trackId.hasValue()) return trackId.error();
        if (!trackIds.insert(trackId.value().value()).second) {
            return parseError("delete undo contains a duplicate track id");
        }
        const auto& clips = track.at("clips");
        if (!clips.is_array()) return parseError("clips is not an array");
        std::vector<Clip> parsedClips;
        parsedClips.reserve(clips.size());
        for (const auto& item : clips) {
            auto parsed = clip(item, assetLoader);
            if (!parsed.hasValue()) return parsed.error();
            if (!clipIds.insert(parsed.value().id().value()).second) {
                return parseError("delete undo contains a duplicate clip id");
            }
            parsedClips.push_back(std::move(parsed).value());
        }
        result.emplace_back(std::move(trackId).value(), std::move(parsedClips));
    }
    return result;
}

}  // namespace

Result<std::unique_ptr<domain::IEditCommand>> EditCommandCodec::decode(
    const domain::EditCommandRecord& record, bool applied) const {
    try {
        const auto payload = Json::parse(record.payload);
        const auto undo = Json::parse(record.undoPayload);
        if (record.type == "SPLIT_CLIP") {
            if (auto exact = exactObject(
                    payload, {"clipId", "rightClipId", "splitNs", "trackId"},
                    "split payload");
                !exact.hasValue()) {
                return exact.error();
            }
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto rightId = idField<ClipId>(payload, "rightClipId");
            auto split = integerField(payload, "splitNs");
            auto original = clip(undo, assetLoader_);
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!rightId.hasValue()) return rightId.error();
            if (!split.hasValue()) return split.error();
            if (!original.hasValue()) return original.error();
            const auto splitAt = TimestampNs{DurationNs{split.value()}};
            if (original.value().id() != clipId.value() ||
                rightId.value() == clipId.value() ||
                splitAt <= original.value().timelineRange().start() ||
                splitAt >= original.value().timelineRange().end()) {
                return parseError("split undo state contradicts payload");
            }
            return domain::SplitClipCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(rightId).value(),
                splitAt,
                std::move(original).value(), applied);
        }
        if (record.type == "TRIM_CLIP") {
            if (auto exact = exactObject(
                    payload, {"boundaryNs", "clipId", "edge", "trackId"},
                    "trim payload");
                !exact.hasValue()) {
                return exact.error();
            }
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto edge = textField(payload, "edge");
            auto boundary = integerField(payload, "boundaryNs");
            auto original = clip(undo, assetLoader_);
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!edge.hasValue()) return edge.error();
            if (!boundary.hasValue()) return boundary.error();
            if (!original.hasValue()) return original.error();
            if (edge.value() != "LEADING" && edge.value() != "TRAILING") {
                return parseError("trim edge is unknown");
            }
            const auto trimAt = TimestampNs{DurationNs{boundary.value()}};
            if (original.value().id() != clipId.value() ||
                trimAt <= original.value().timelineRange().start() ||
                trimAt >= original.value().timelineRange().end()) {
                return parseError("trim undo state contradicts payload");
            }
            return domain::TrimClipCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(),
                edge.value() == "LEADING" ? TrimEdge::Leading : TrimEdge::Trailing,
                trimAt,
                std::move(original).value(), applied);
        }
        if (record.type == "SET_VISUAL_TRANSFORM") {
            if (auto exact = exactObject(
                    payload, {"clipId", "trackId", "version", "visual"},
                    "visual transform payload"); !exact.hasValue()) {
                return exact.error();
            }
            if (auto exact = exactObject(undo, {"previous"},
                                         "visual transform undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto value = visual(payload.at("visual"));
            auto previous = visual(undo.at("previous"));
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!value.hasValue()) return value.error();
            if (!previous.hasValue()) return previous.error();
            return domain::SetVisualTransformCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(value).value(),
                std::move(previous).value(), applied);
        }
        if (record.type == "SET_AUDIO_ENVELOPE") {
            if (auto exact = exactObject(
                    payload, {"audio", "clipDurationNs", "clipId", "trackId",
                              "version"}, "audio envelope payload");
                !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(undo, {"previous"},
                                         "audio envelope undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto duration = integerField(payload, "clipDurationNs");
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            if (!version.hasValue()) return version.error();
            if (!duration.hasValue()) return duration.error();
            if (duration.value() <= 0) return parseError("clipDurationNs is not positive");
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            const DurationNs clipDuration{duration.value()};
            auto value = audio(payload.at("audio"), clipDuration);
            auto previous = audio(undo.at("previous"), clipDuration);
            if (!value.hasValue()) return value.error();
            if (!previous.hasValue()) return previous.error();
            return domain::SetAudioEnvelopeCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(value).value(),
                std::move(previous).value(), clipDuration, applied);
        }
        if (record.type == "ADD_TITLE") {
            if (auto exact = exactObject(
                    payload, {"clip", "trackId", "trackName", "version"},
                    "add title payload"); !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(undo, {"createdTrack"},
                                         "add title undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto trackName = textField(payload, "trackName");
            auto titleClip = generatedClip(payload.at("clip"));
            auto createdTrack = boolField(undo, "createdTrack");
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!trackName.hasValue()) return trackName.error();
            if (!titleClip.hasValue()) return titleClip.error();
            if (!createdTrack.hasValue()) return createdTrack.error();
            if (trackName.value().empty() ||
                trackId.value().value() != "title-1" ||
                titleClip.value().kind() != domain::ClipKind::Title) {
                return parseError("add title stable identities are contradictory");
            }
            return domain::AddTitleCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(trackName).value(), std::move(titleClip).value(),
                createdTrack.value(), applied);
        }
        if (record.type == "EDIT_TITLE") {
            if (auto exact = exactObject(
                    payload, {"clipId", "title", "trackId", "version"},
                    "edit title payload"); !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(undo, {"previous"}, "edit title undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto value = titlePayload(payload.at("title"));
            auto previous = titlePayload(undo.at("previous"));
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!value.hasValue()) return value.error();
            if (!previous.hasValue()) return previous.error();
            if (trackId.value().value() != "title-1") {
                return parseError("edit title track identity is not stable");
            }
            return domain::EditTitleCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(value).value(),
                std::move(previous).value(), applied);
        }
        if (record.type == "REMOVE_GENERATED_CLIP") {
            if (auto exact = exactObject(
                    payload, {"clipId", "trackId", "version"},
                    "remove generated clip payload");
                !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(undo, {"clip"},
                                         "remove generated clip undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto removed = generatedClip(undo.at("clip"));
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!removed.hasValue()) return removed.error();
            if (removed.value().id() != clipId.value()) {
                return parseError("removed generated clip identity contradicts payload");
            }
            const auto expectedTrack =
                removed.value().kind() == domain::ClipKind::Title
                    ? "title-1" : "caption-1";
            if (trackId.value().value() != expectedTrack) {
                return parseError("removed generated clip track contradicts kind");
            }
            return domain::RemoveGeneratedClipCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(removed).value(), applied);
        }
        if (record.type == "ADD_CAPTION_CUE") {
            if (auto exact = exactObject(
                    payload, {"clipDurationNs", "clipId", "clipStartNs", "cue",
                              "enabled", "trackId", "trackName", "version", "visual"},
                    "add caption cue payload");
                !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(
                    undo, {"createdClip", "createdTrack"},
                    "add caption cue undo"); !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto trackName = textField(payload, "trackName");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto start = integerField(payload, "clipStartNs");
            auto duration = integerField(payload, "clipDurationNs");
            auto enabled = boolField(payload, "enabled");
            auto parsedVisual = visual(payload.at("visual"));
            auto cue = captionCue(payload.at("cue"));
            auto createdClip = boolField(undo, "createdClip");
            auto createdTrack = boolField(undo, "createdTrack");
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!trackName.hasValue()) return trackName.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!start.hasValue()) return start.error();
            if (!duration.hasValue()) return duration.error();
            if (!enabled.hasValue()) return enabled.error();
            if (!parsedVisual.hasValue()) return parsedVisual.error();
            if (!cue.hasValue()) return cue.error();
            if (!createdClip.hasValue()) return createdClip.error();
            if (!createdTrack.hasValue()) return createdTrack.error();
            auto range = TimeRange::create(
                TimestampNs{DurationNs{start.value()}}, DurationNs{duration.value()});
            if (!range.hasValue()) return parseError(range.error().message());
            if (trackName.value().empty() ||
                trackId.value().value() != "caption-1" ||
                cue.value().endOffset() > range.value().duration() ||
                (createdTrack.value() && !createdClip.value())) {
                return parseError("add caption cue undo state contradicts payload");
            }
            return domain::AddCaptionCueCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(trackName).value(), std::move(clipId).value(),
                range.value(), enabled.value(), std::move(parsedVisual).value(),
                std::move(cue).value(), createdTrack.value(),
                createdClip.value(), applied);
        }
        if (record.type == "EDIT_CAPTION_CUE") {
            if (auto exact = exactObject(
                    payload, {"clipId", "cueId", "replacement", "trackId", "version"},
                    "edit caption cue payload"); !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(undo, {"previous"},
                                         "edit caption cue undo");
                !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto cueId = idField<CueId>(payload, "cueId");
            auto replacement = captionCue(payload.at("replacement"));
            auto previous = captionCue(undo.at("previous"));
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!cueId.hasValue()) return cueId.error();
            if (!replacement.hasValue()) return replacement.error();
            if (!previous.hasValue()) return previous.error();
            if (replacement.value().id() != cueId.value() ||
                previous.value().id() != cueId.value() ||
                trackId.value().value() != "caption-1") {
                return parseError("edit caption cue identities contradict payload");
            }
            return domain::EditCaptionCueCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(cueId).value(),
                std::move(replacement).value(), std::move(previous).value(), applied);
        }
        if (record.type == "REMOVE_CAPTION_CUE") {
            if (auto exact = exactObject(
                    payload, {"clipId", "cueId", "trackId", "version"},
                    "remove caption cue payload"); !exact.hasValue()) return exact.error();
            if (auto exact = exactObject(
                    undo, {"previous", "removedClip"},
                    "remove caption cue undo"); !exact.hasValue()) return exact.error();
            auto version = versionOne(payload);
            auto trackId = idField<TrackId>(payload, "trackId");
            auto clipId = idField<ClipId>(payload, "clipId");
            auto cueId = idField<CueId>(payload, "cueId");
            auto previous = captionCue(undo.at("previous"));
            if (!version.hasValue()) return version.error();
            if (!trackId.hasValue()) return trackId.error();
            if (!clipId.hasValue()) return clipId.error();
            if (!cueId.hasValue()) return cueId.error();
            if (!previous.hasValue()) return previous.error();
            if (previous.value().id() != cueId.value()) {
                return parseError("removed caption cue identity contradicts payload");
            }
            if (trackId.value().value() != "caption-1") {
                return parseError("remove caption cue track identity is not stable");
            }
            std::optional<Clip> removedClip;
            if (!undo.at("removedClip").is_null()) {
                auto parsed = generatedClip(undo.at("removedClip"));
                if (!parsed.hasValue()) return parsed.error();
                if (parsed.value().id() != clipId.value() ||
                    parsed.value().kind() != domain::ClipKind::Caption ||
                    parsed.value().captionCues().size() != 1 ||
                    parsed.value().captionCues().front() != previous.value()) {
                    return parseError("removed caption clip contradicts cue payload");
                }
                removedClip = std::move(parsed).value();
            }
            return domain::RemoveCaptionCueCommand::rehydrate(
                record.commandId, std::move(trackId).value(),
                std::move(clipId).value(), std::move(cueId).value(),
                std::move(previous).value(), std::move(removedClip), applied);
        }
        if (record.type == "DELETE_RANGE") {
            if (auto exact = exactObject(
                    payload,
                    {"durationNs", "rightClipIds", "ripple", "startNs"},
                    "delete payload");
                !exact.hasValue()) {
                return exact.error();
            }
            auto start = integerField(payload, "startNs");
            auto duration = integerField(payload, "durationNs");
            auto ripple = boolField(payload, "ripple");
            if (!start.hasValue()) return start.error();
            if (!duration.hasValue()) return duration.error();
            if (!ripple.hasValue()) return ripple.error();
            auto deletion = TimeRange::create(
                TimestampNs{DurationNs{start.value()}}, DurationNs{duration.value()});
            if (!deletion.hasValue()) return parseError(deletion.error().message());
            const auto& ids = payload.at("rightClipIds");
            if (!ids.is_array()) return parseError("rightClipIds is not an array");
            std::vector<ClipId> rightIds;
            rightIds.reserve(ids.size());
            std::set<std::string> rightIdValues;
            for (const auto& item : ids) {
                if (!item.is_string()) return parseError("rightClipId is not text");
                auto id = ClipId::create(item.get<std::string>());
                if (!id.hasValue()) return parseError("rightClipId is empty");
                if (!rightIdValues.insert(id.value().value()).second) {
                    return parseError("rightClipIds contains a duplicate id");
                }
                rightIds.push_back(std::move(id).value());
            }
            auto tracks = previousTracks(undo, assetLoader_);
            if (!tracks.hasValue()) return tracks.error();
            std::size_t requiredRightIds = 0;
            for (const auto& [trackId, clips] : tracks.value()) {
                static_cast<void>(trackId);
                for (const auto& original : clips) {
                    if (rightIdValues.contains(original.id().value())) {
                        return parseError(
                            "rightClipId collides with a restored clip id");
                    }
                    if (original.timelineRange().start() < deletion.value().start() &&
                        original.timelineRange().end() > deletion.value().end()) {
                        ++requiredRightIds;
                    }
                }
            }
            if (requiredRightIds != rightIds.size()) {
                return parseError(
                    "rightClipIds count contradicts delete undo state");
            }
            return DeleteRangeCommand::rehydrate(
                record.commandId, deletion.value(), ripple.value(),
                std::move(rightIds), std::move(tracks).value(), applied);
        }
        return parseError("unknown command type " + record.type);
    } catch (const std::exception& exception) {
        return parseError(exception.what());
    }
}

}  // namespace creator::project_store::internal
