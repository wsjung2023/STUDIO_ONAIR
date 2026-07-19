#include "cut_suggest/CutSuggestionSerializer.h"

#include "cut_suggest/CutReason.h"

#include <string>

namespace creator::cut_suggest {

using core::Result;

Result<nlohmann::json> CutSuggestionSerializer::toJson(
    const CutSuggestion& suggestion) {
    nlohmann::json json;
    json["schemaVersion"] = kSchemaVersion;

    nlohmann::json span;
    span["startNs"] = suggestion.span().start().time_since_epoch().count();
    span["durationNs"] = suggestion.span().duration().count();
    json["span"] = std::move(span);

    json["reason"] = std::string{toString(suggestion.reason())};
    json["score"] = suggestion.score();
    if (suggestion.label().has_value()) {
        json["label"] = *suggestion.label();
    }
    return json;
}

Result<nlohmann::json> CutSuggestionSerializer::toJsonArray(
    std::span<const CutSuggestion> suggestions) {
    nlohmann::json array = nlohmann::json::array();
    for (const CutSuggestion& suggestion : suggestions) {
        auto document = toJson(suggestion);
        if (!document) return document.error();
        array.push_back(std::move(document).value());
    }
    return array;
}

}  // namespace creator::cut_suggest
