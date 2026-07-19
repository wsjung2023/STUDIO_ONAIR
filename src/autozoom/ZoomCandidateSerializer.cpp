#include "autozoom/ZoomCandidateSerializer.h"

namespace creator::autozoom {

core::Result<nlohmann::json> ZoomCandidateSerializer::toJson(const ZoomCandidate& candidate) {
    nlohmann::json json;
    json["schemaVersion"] = kSchemaVersion;

    nlohmann::json span;
    span["startNs"] = candidate.span().start().time_since_epoch().count();
    span["durationNs"] = candidate.span().duration().count();
    json["span"] = std::move(span);

    nlohmann::json region;
    region["centerX"] = candidate.region().center().x();
    region["centerY"] = candidate.region().center().y();
    region["zoomFactor"] = candidate.region().zoomFactor();
    json["region"] = std::move(region);

    json["score"] = candidate.score();
    return json;
}

}  // namespace creator::autozoom
