#include "domain/EditCommandJson.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace creator::domain::internal {
namespace {

std::string jsonString(std::string_view value) {
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

std::string number(double value) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
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

std::string serializeClip(const Clip& clip) {
    return "{\"assetId\":" + jsonString(clip.assetId()->value()) +
           ",\"audio\":" + audioJson(clip.audioEnvelope()) +
           ",\"enabled\":" + (clip.enabled() ? "true" : "false") +
           ",\"id\":" + jsonString(clip.id().value()) +
           ",\"mediaKind\":" +
           jsonString(clip.mediaKind() == MediaKind::Video
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
           ",\"visual\":" + visualJson(clip.visualTransform()) + "}";
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
        json += "],\"trackId\":" + jsonString(trackId.value()) + "}";
    }
    json += "]}";
    return json;
}

}  // namespace creator::domain::internal
