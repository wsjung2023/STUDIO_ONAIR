#include "cursor_emphasis/EmphasisPlanSerializer.h"

#include "cursor/CursorButton.h"

#include <string>

namespace creator::cursor_emphasis {

core::Result<nlohmann::json> EmphasisPlanSerializer::toJson(const EmphasisPlan& plan) {
    nlohmann::json json;
    json["schemaVersion"] = kSchemaVersion;

    nlohmann::json clicks = nlohmann::json::array();
    for (const ClickEmphasis& emphasis : plan.clicks()) {
        nlohmann::json entry;
        entry["startNs"] = emphasis.startNs().time_since_epoch().count();
        entry["durationNs"] = emphasis.duration().count();
        entry["x"] = emphasis.position().x();
        entry["y"] = emphasis.position().y();
        entry["button"] = std::string{cursor::toString(emphasis.button())};
        entry["style"] = std::string{toString(emphasis.style())};
        entry["radius"] = emphasis.radius();
        clicks.push_back(std::move(entry));
    }
    json["clicks"] = std::move(clicks);

    nlohmann::json hideSpans = nlohmann::json::array();
    for (const CursorHideSpan& hide : plan.hideSpans()) {
        nlohmann::json entry;
        entry["startNs"] = hide.span().start().time_since_epoch().count();
        entry["durationNs"] = hide.span().duration().count();
        entry["reason"] = std::string{toString(hide.reason())};
        hideSpans.push_back(std::move(entry));
    }
    json["hideSpans"] = std::move(hideSpans);

    return json;
}

}  // namespace creator::cursor_emphasis
