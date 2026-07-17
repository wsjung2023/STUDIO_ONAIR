#include "domain/EditCommandJson.h"

#include <array>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace creator::domain::internal {

std::string serializeJsonString(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u00" << std::hex << std::setw(2)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec;
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
    return output.str();
}

namespace {

std::string number(double value) {
    if (value == 0.0) return "0";
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (error != std::errc{}) return {};
    return std::string{buffer.data(), end};
}

std::string visualJson(const std::optional<VisualTransform>& visual) {
    if (!visual.has_value()) return "null";
    const auto& v = *visual;
    return "{\"cropBottom\":" + number(v.cropBottom()) +
           ",\"cropLeft\":" + number(v.cropLeft()) +
           ",\"cropRight\":" + number(v.cropRight()) +
           ",\"cropTop\":" + number(v.cropTop()) +
           ",\"height\":" + number(v.height()) +
           ",\"opacity\":" + number(v.opacity()) +
           ",\"rotationDegrees\":" + number(v.rotationDegrees()) +
           ",\"scaleX\":" + number(v.scaleX()) +
           ",\"scaleY\":" + number(v.scaleY()) +
           ",\"width\":" + number(v.width()) +
           ",\"x\":" + number(v.x()) +
           ",\"y\":" + number(v.y()) +
           ",\"zOrder\":" + std::to_string(v.zOrder()) + "}";
}

std::string audioJson(const std::optional<AudioEnvelope>& audio) {
    if (!audio.has_value()) return "null";
    const auto& a = *audio;
    return "{\"fadeInNs\":" + std::to_string(a.fadeIn().count()) +
           ",\"fadeOutNs\":" + std::to_string(a.fadeOut().count()) +
           ",\"gainDb\":" + number(a.gainDb()) + "}";
}

}  // namespace

std::string serializeVisualTransform(
    const std::optional<VisualTransform>& visual) {
    return visualJson(visual);
}

std::string serializeAudioEnvelope(
    const std::optional<AudioEnvelope>& audio) {
    return audioJson(audio);
}

std::string serializeTitlePayload(const TitlePayload& title) {
    const auto alignment = title.alignment() == TextAlignment::Left
                               ? "LEFT"
                               : (title.alignment() == TextAlignment::Center
                                      ? "CENTER"
                                      : "RIGHT");
    return "{\"alignment\":" + serializeJsonString(alignment) +
           ",\"background\":" + serializeJsonString(title.background().toString()) +
           ",\"fontFamily\":" + serializeJsonString(title.fontFamily()) +
           ",\"foreground\":" + serializeJsonString(title.foreground().toString()) +
           ",\"text\":" + serializeJsonString(title.text()) +
           ",\"x\":" + number(title.x()) +
           ",\"y\":" + number(title.y()) + "}";
}

std::string serializeCaptionCue(const CaptionCue& cue) {
    return "{\"durationNs\":" + std::to_string(cue.duration().count()) +
           ",\"id\":" + serializeJsonString(cue.id().value()) +
           ",\"startOffsetNs\":" +
           std::to_string(cue.startOffset().count()) +
           ",\"text\":" + serializeJsonString(cue.text()) + "}";
}

std::string serializeGeneratedClip(const Clip& clip) {
    std::string cues{"["};
    for (std::size_t index = 0; index < clip.captionCues().size(); ++index) {
        if (index != 0) cues.push_back(',');
        cues += serializeCaptionCue(clip.captionCues()[index]);
    }
    cues.push_back(']');
    const auto kind = clip.kind() == ClipKind::Title ? "TITLE" : "CAPTION";
    const auto title = clip.titlePayload().has_value()
                           ? serializeTitlePayload(*clip.titlePayload())
                           : "null";
    return "{\"captionCues\":" + cues +
           ",\"clipKind\":" + serializeJsonString(kind) +
           ",\"enabled\":" + (clip.enabled() ? "true" : "false") +
           ",\"id\":" + serializeJsonString(clip.id().value()) +
           ",\"sourceDurationNs\":" +
           std::to_string(clip.sourceRange().duration().count()) +
           ",\"sourceStartNs\":" +
           std::to_string(clip.sourceRange().start().time_since_epoch().count()) +
           ",\"timelineDurationNs\":" +
           std::to_string(clip.timelineRange().duration().count()) +
           ",\"timelineStartNs\":" +
           std::to_string(clip.timelineRange().start().time_since_epoch().count()) +
           ",\"title\":" + title +
           ",\"visual\":" + serializeVisualTransform(clip.visualTransform()) + "}";
}

std::string serializeClip(const Clip& clip) {
    if (clip.kind() != ClipKind::Asset) return serializeGeneratedClip(clip);
    return "{\"assetId\":" + serializeJsonString(clip.assetId()->value()) +
           ",\"audio\":" + serializeAudioEnvelope(clip.audioEnvelope()) +
           ",\"enabled\":" + (clip.enabled() ? "true" : "false") +
           ",\"id\":" + serializeJsonString(clip.id().value()) +
           ",\"mediaKind\":" +
           serializeJsonString(clip.mediaKind() == MediaKind::Video
                          ? "VIDEO"
                          : (clip.mediaKind() == MediaKind::Audio ? "AUDIO"
                                                                  : "IMAGE")) +
           ",\"sourceDurationNs\":" +
           std::to_string(clip.sourceRange().duration().count()) +
           ",\"sourceStartNs\":" +
           std::to_string(clip.sourceRange().start().time_since_epoch().count()) +
           ",\"timelineDurationNs\":" +
           std::to_string(clip.timelineRange().duration().count()) +
           ",\"timelineStartNs\":" +
           std::to_string(clip.timelineRange().start().time_since_epoch().count()) +
           ",\"visual\":" + serializeVisualTransform(clip.visualTransform()) + "}";
}

std::string serializeTrackClips(const std::vector<TrackClips>& tracks) {
    std::string json{"{\"tracks\":["};
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (trackIndex != 0) json.push_back(',');
        json += "{\"clips\":[";
        const auto& [trackId, clips] = tracks[trackIndex];
        for (std::size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex) {
            if (clipIndex != 0) json.push_back(',');
            json += serializeClip(clips[clipIndex]);
        }
        json += "],\"trackId\":" + serializeJsonString(trackId.value()) + "}";
    }
    json += "]}";
    return json;
}

}  // namespace creator::domain::internal
