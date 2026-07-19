#include "transcription/TranscriptSerializer.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/TimelineTypes.h"
#include "transcription/TranscriptSegment.h"
#include "transcription/TranscriptWord.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace creator::transcription {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

nlohmann::json timeRangeToJson(const domain::TimeRange& range) {
    nlohmann::json object;
    object["startNs"] = range.start().time_since_epoch().count();
    object["durationNs"] = range.duration().count();
    return object;
}

Result<domain::TimeRange> timeRangeFromJson(const nlohmann::json& object) {
    if (!object.is_object() || !object.contains("startNs") || !object.contains("durationNs") ||
        !object["startNs"].is_number_integer() || !object["durationNs"].is_number_integer()) {
        return AppError{ErrorCode::ParseFailure, "transcript timeRange is malformed"};
    }
    const auto start = object["startNs"].get<std::int64_t>();
    const auto duration = object["durationNs"].get<std::int64_t>();
    return domain::TimeRange::create(core::TimestampNs{core::DurationNs{start}},
                                     core::DurationNs{duration});
}

Result<TranscriptWord> wordFromJson(const nlohmann::json& object) {
    if (!object.is_object() || !object.contains("text") || !object["text"].is_string() ||
        !object.contains("confidence") || !object["confidence"].is_number() ||
        !object.contains("timeRange")) {
        return AppError{ErrorCode::ParseFailure, "transcript word is malformed"};
    }
    auto range = timeRangeFromJson(object["timeRange"]);
    if (!range.hasValue()) return range.error();
    return TranscriptWord::create(object["text"].get<std::string>(), range.value(),
                                  object["confidence"].get<double>());
}

Result<TranscriptSegment> segmentFromJson(const nlohmann::json& object) {
    if (!object.is_object() || !object.contains("text") || !object["text"].is_string() ||
        !object.contains("timeRange")) {
        return AppError{ErrorCode::ParseFailure, "transcript segment is malformed"};
    }
    auto range = timeRangeFromJson(object["timeRange"]);
    if (!range.hasValue()) return range.error();

    std::vector<TranscriptWord> words;
    if (object.contains("words")) {
        if (!object["words"].is_array()) {
            return AppError{ErrorCode::ParseFailure, "transcript segment words must be an array"};
        }
        for (const auto& wordJson : object["words"]) {
            auto word = wordFromJson(wordJson);
            if (!word.hasValue()) return word.error();
            words.push_back(std::move(word).value());
        }
    }

    std::optional<std::string> speaker;
    if (object.contains("speaker")) {
        if (!object["speaker"].is_string()) {
            return AppError{ErrorCode::ParseFailure, "transcript segment speaker must be a string"};
        }
        speaker = object["speaker"].get<std::string>();
    }

    return TranscriptSegment::create(object["text"].get<std::string>(), range.value(),
                                     std::move(words), std::move(speaker));
}

}  // namespace

nlohmann::json TranscriptSerializer::toJson(const Transcript& transcript) {
    nlohmann::json document;
    document["schemaVersion"] = kSchemaVersion;
    document["language"] = transcript.languageTag();
    document["sourceId"] = transcript.sourceId().value();

    nlohmann::json segments = nlohmann::json::array();
    for (const TranscriptSegment& segment : transcript.segments()) {
        nlohmann::json segmentJson;
        segmentJson["text"] = segment.text();
        segmentJson["timeRange"] = timeRangeToJson(segment.range());
        if (segment.speaker().has_value()) {
            segmentJson["speaker"] = *segment.speaker();
        }

        nlohmann::json words = nlohmann::json::array();
        for (const TranscriptWord& word : segment.words()) {
            nlohmann::json wordJson;
            wordJson["text"] = word.text();
            wordJson["timeRange"] = timeRangeToJson(word.range());
            wordJson["confidence"] = word.confidence();
            words.push_back(std::move(wordJson));
        }
        segmentJson["words"] = std::move(words);
        segments.push_back(std::move(segmentJson));
    }
    document["segments"] = std::move(segments);
    return document;
}

Result<Transcript> TranscriptSerializer::fromJson(const nlohmann::json& document) {
    try {
        if (!document.is_object()) {
            return AppError{ErrorCode::ParseFailure, "transcript document must be a JSON object"};
        }
        if (!document.contains("schemaVersion") || !document["schemaVersion"].is_number_integer()) {
            return AppError{ErrorCode::ParseFailure,
                            "transcript document is missing an integer schemaVersion"};
        }
        const int version = document["schemaVersion"].get<int>();
        if (version != kSchemaVersion) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "unsupported transcript schema version " + std::to_string(version)};
        }
        if (!document.contains("language") || !document["language"].is_string()) {
            return AppError{ErrorCode::ParseFailure, "transcript document is missing a language"};
        }
        if (!document.contains("sourceId") || !document["sourceId"].is_string()) {
            return AppError{ErrorCode::ParseFailure, "transcript document is missing a sourceId"};
        }
        if (!document.contains("segments") || !document["segments"].is_array()) {
            return AppError{ErrorCode::ParseFailure, "transcript document segments must be an array"};
        }

        auto sourceId = domain::SourceId::create(document["sourceId"].get<std::string>());
        if (!sourceId.hasValue()) return sourceId.error();

        std::vector<TranscriptSegment> segments;
        for (const auto& segmentJson : document["segments"]) {
            auto segment = segmentFromJson(segmentJson);
            if (!segment.hasValue()) return segment.error();
            segments.push_back(std::move(segment).value());
        }

        return Transcript::create(std::move(segments), document["language"].get<std::string>(),
                                  std::move(sourceId).value());
    } catch (const std::exception& error) {
        return AppError{ErrorCode::ParseFailure,
                        std::string{"transcript document could not be parsed: "} + error.what()};
    }
}

}  // namespace creator::transcription
