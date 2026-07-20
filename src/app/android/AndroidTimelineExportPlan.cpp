#include "app/android/AndroidTimelineExportPlan.h"

#include "app/RecentProjectRegistry.h"
#include "core/AppError.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <unordered_map>

namespace creator::app::android {
namespace {

using core::AppError;
using core::ErrorCode;

AppError invalid(std::string message) {
    return {ErrorCode::InvalidState, std::move(message)};
}

qint64 microseconds(core::TimestampNs value) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               value.time_since_epoch())
        .count();
}

qint64 microseconds(core::DurationNs value) {
    return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

bool escapesRoot(const std::filesystem::path& relative) {
    return relative.empty() || relative.is_absolute() ||
           (!relative.empty() && *relative.begin() == "..");
}

core::Result<std::filesystem::path> resolveInput(
    const std::filesystem::path& root,
    const std::filesystem::path& relative, const char* description) {
    if (relative.empty() || relative.is_absolute()) {
        return invalid(std::string{description} + " path is not package-relative");
    }
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error || canonicalRoot.empty()) {
        return invalid("Android export media root could not be canonicalized");
    }
    const auto candidate =
        std::filesystem::weakly_canonical(canonicalRoot / relative, error);
    if (error || !std::filesystem::is_regular_file(candidate, error) || error) {
        return invalid(std::string{description} + " file is unavailable");
    }
    const auto inside = candidate.lexically_relative(canonicalRoot);
    if (escapesRoot(inside)) {
        return invalid(std::string{description} + " escapes the project package");
    }
    return candidate;
}

QJsonObject visualTransform(const std::optional<domain::VisualTransform>& value) {
    if (!value) {
        return {{QStringLiteral("x"), 0.0},
                {QStringLiteral("y"), 0.0},
                {QStringLiteral("width"), 1.0},
                {QStringLiteral("height"), 1.0},
                {QStringLiteral("scaleX"), 1.0},
                {QStringLiteral("scaleY"), 1.0},
                {QStringLiteral("rotation"), 0.0},
                {QStringLiteral("cropLeft"), 0.0},
                {QStringLiteral("cropTop"), 0.0},
                {QStringLiteral("cropRight"), 0.0},
                {QStringLiteral("cropBottom"), 0.0},
                {QStringLiteral("opacity"), 1.0},
                {QStringLiteral("zOrder"), 0}};
    }
    return {{QStringLiteral("x"), value->x()},
            {QStringLiteral("y"), value->y()},
            {QStringLiteral("width"), value->width()},
            {QStringLiteral("height"), value->height()},
            {QStringLiteral("scaleX"), value->scaleX()},
            {QStringLiteral("scaleY"), value->scaleY()},
            {QStringLiteral("rotation"), value->rotationDegrees()},
            {QStringLiteral("cropLeft"), value->cropLeft()},
            {QStringLiteral("cropTop"), value->cropTop()},
            {QStringLiteral("cropRight"), value->cropRight()},
            {QStringLiteral("cropBottom"), value->cropBottom()},
            {QStringLiteral("opacity"), value->opacity()},
            {QStringLiteral("zOrder"), value->zOrder()}};
}

const edit_engine::GeneratedOverlayDescriptor* findOverlay(
    const edit_engine::TimelineSnapshot& snapshot,
    const domain::ClipId& owner,
    const std::optional<domain::CueId>& cue) {
    const edit_engine::GeneratedOverlayDescriptor* match = nullptr;
    for (const auto& descriptor : snapshot.generatedOverlays) {
        if (descriptor.ownerClipId() != owner || descriptor.cueId() != cue) {
            continue;
        }
        if (match != nullptr) return nullptr;
        match = &descriptor;
    }
    return match;
}

QJsonObject timing(core::TimestampNs sourceStart,
                   core::TimestampNs timelineStart,
                   core::DurationNs duration) {
    return {{QStringLiteral("sourceStartUs"), microseconds(sourceStart)},
            {QStringLiteral("timelineStartUs"), microseconds(timelineStart)},
            {QStringLiteral("durationUs"), microseconds(duration)}};
}

}  // namespace

core::Result<AndroidTimelineExportPlan> buildAndroidTimelineExportPlan(
    const edit_engine::RenderRequest& request,
    const std::filesystem::path& partialDestination) {
    const auto& snapshot = request.snapshot();
    if (auto validated = edit_engine::validateTimelineSnapshot(snapshot);
        !validated.hasValue()) {
        return validated.error();
    }
    if (partialDestination.empty() || !partialDestination.is_absolute() ||
        partialDestination.extension() != ".mp4") {
        return invalid("Android export partial destination is invalid");
    }
    const auto rate = request.preset().frameRate();
    if (rate.denominator() == 0 || rate.numerator() == 0) {
        return invalid("Android export frame rate is invalid");
    }

    std::unordered_map<std::string, const domain::MediaAsset*> assets;
    for (const auto& asset : snapshot.assets) {
        if (!assets.emplace(asset.id().value(), &asset).second) {
            return invalid("Android export media catalog contains duplicate ids");
        }
    }

    QJsonArray visuals;
    QJsonArray audio;
    core::TimestampNs timelineEnd{};
    int trackOrder = 0;
    for (const auto& track : snapshot.timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            timelineEnd = std::max(timelineEnd, clip.timelineRange().end());
            if (!track.enabled() || !clip.enabled()) continue;

            if (clip.kind() == domain::ClipKind::Asset) {
                if (!clip.assetId()) {
                    return invalid("Android export asset clip has no asset id");
                }
                const auto found = assets.find(clip.assetId()->value());
                if (found == assets.end()) {
                    return invalid("Android export clip references unknown media");
                }
                const auto& asset = *found->second;
                if (asset.availability() != domain::AssetAvailability::Available) {
                    return invalid("Android export media is offline");
                }
                auto source = resolveInput(snapshot.mediaRoot,
                                           std::filesystem::path{
                                               std::u8string{asset.relativePath().begin(),
                                                             asset.relativePath().end()}},
                                           "Android export media");
                if (!source.hasValue()) return source.error();
                const auto duration = std::min(clip.sourceRange().duration(),
                                               clip.timelineRange().duration());
                if (duration.count() <= 0) {
                    return invalid("Android export clip has no complete duration");
                }
                if (track.kind() == domain::TrackKind::Video) {
                    if (asset.kind() != domain::MediaKind::Video &&
                        asset.kind() != domain::MediaKind::Image) {
                        return invalid("Android visual track contains non-visual media");
                    }
                    auto row = timing(clip.sourceRange().start(),
                                      clip.timelineRange().start(), duration);
                    row.insert(QStringLiteral("path"),
                               qStringFromPath(source.value()));
                    row.insert(QStringLiteral("kind"),
                               asset.kind() == domain::MediaKind::Video
                                   ? QStringLiteral("video")
                                   : QStringLiteral("image"));
                    row.insert(QStringLiteral("identity"),
                               QString::fromStdString(clip.id().value()));
                    row.insert(QStringLiteral("trackOrder"), trackOrder);
                    row.insert(QStringLiteral("transform"),
                               visualTransform(clip.visualTransform()));
                    visuals.append(row);
                }
                if (clip.hasAudio()) {
                    if (!asset.audio()) {
                        return invalid("Android audible clip has no audio metadata");
                    }
                    auto row = timing(clip.sourceRange().start(),
                                      clip.timelineRange().start(), duration);
                    row.insert(QStringLiteral("path"),
                               qStringFromPath(source.value()));
                    const auto envelope = clip.audioEnvelope();
                    row.insert(QStringLiteral("gainDb"),
                               envelope ? envelope->gainDb() : 0.0);
                    row.insert(QStringLiteral("fadeInUs"),
                               envelope ? microseconds(envelope->fadeIn()) : 0);
                    row.insert(QStringLiteral("fadeOutUs"),
                               envelope ? microseconds(envelope->fadeOut()) : 0);
                    audio.append(row);
                }
                continue;
            }

            const auto addGenerated = [&](
                                          const std::optional<domain::CueId>& cue,
                                          const domain::TimeRange& range)
                -> core::Result<void> {
                const auto* overlay = findOverlay(snapshot, clip.id(), cue);
                if (!overlay) {
                    return invalid("Android export generated overlay is missing or duplicated");
                }
                auto source = resolveInput(snapshot.mediaRoot,
                                           overlay->rasterPath(),
                                           "Android export generated overlay");
                if (!source.hasValue()) return source.error();
                auto row = timing(core::TimestampNs{}, range.start(),
                                  range.duration());
                row.insert(QStringLiteral("path"),
                           qStringFromPath(source.value()));
                row.insert(QStringLiteral("kind"), QStringLiteral("image"));
                row.insert(QStringLiteral("identity"),
                           QString::fromStdString(
                               cue ? cue->value() : clip.id().value()));
                row.insert(QStringLiteral("trackOrder"), trackOrder);
                row.insert(QStringLiteral("transform"),
                           visualTransform(std::nullopt));
                if (clip.visualTransform()) {
                    auto transform = row.value(QStringLiteral("transform")).toObject();
                    transform.insert(QStringLiteral("zOrder"),
                                     clip.visualTransform()->zOrder());
                    row.insert(QStringLiteral("transform"), transform);
                }
                visuals.append(row);
                return core::ok();
            };

            if (clip.kind() == domain::ClipKind::Title) {
                auto added = addGenerated(std::nullopt, clip.timelineRange());
                if (!added.hasValue()) return added.error();
            } else if (clip.kind() == domain::ClipKind::Caption) {
                for (const auto& cue : clip.captionCues()) {
                    auto cueRange = domain::TimeRange::create(
                        clip.timelineRange().start() + cue.startOffset(),
                        cue.duration());
                    if (!cueRange.hasValue()) return cueRange.error();
                    auto added = addGenerated(cue.id(), cueRange.value());
                    if (!added.hasValue()) return added.error();
                }
            } else {
                return invalid("Android export encountered an unknown clip kind");
            }
        }
        ++trackOrder;
    }

    const auto duration = timelineEnd.time_since_epoch();
    if (duration.count() <= 0) {
        return invalid("Android export timeline has no duration");
    }
    QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("destination"), qStringFromPath(partialDestination)},
        {QStringLiteral("width"), static_cast<qint64>(request.preset().width())},
        {QStringLiteral("height"), static_cast<qint64>(request.preset().height())},
        {QStringLiteral("frameRateNumerator"),
         static_cast<qint64>(rate.numerator())},
        {QStringLiteral("frameRateDenominator"),
         static_cast<qint64>(rate.denominator())},
        {QStringLiteral("videoBitrate"),
         static_cast<qint64>(request.preset().videoBitrate())},
        {QStringLiteral("audioBitrate"),
         static_cast<qint64>(request.preset().audioBitrate())},
        {QStringLiteral("durationUs"), microseconds(duration)},
        {QStringLiteral("visualClips"), visuals},
        {QStringLiteral("audioClips"), audio}};
    return AndroidTimelineExportPlan{
        .json = QJsonDocument{root}.toJson(QJsonDocument::Compact),
        .duration = duration,
        .visualClipCount = static_cast<std::size_t>(visuals.size()),
        .audioClipCount = static_cast<std::size_t>(audio.size())};
}

}  // namespace creator::app::android
