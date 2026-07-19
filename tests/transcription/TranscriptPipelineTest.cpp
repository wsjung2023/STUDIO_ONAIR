#include "transcription/AudioInput.h"
#include "transcription/FakeTranscriptionProvider.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptSerializer.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::FakeTranscriptionProvider;
using creator::transcription::TranscriptionOptions;
using creator::transcription::TranscriptSerializer;

// Mirrors project_store::ManifestSchemaValidator: compile the committed schema
// and record the first validation error. Test-only; the shipped module does not
// link json-schema-validator (schema conformance is guaranteed by construction).
class RecordingErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& instance, const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        if (!failed_) {
            failed_ = true;
            pointer_ = pointer.to_string();
            message_ = message;
        }
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& pointer() const noexcept { return pointer_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    bool failed_{false};
    std::string pointer_;
    std::string message_;
};

const nlohmann::json_schema::json_validator& transcriptValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream file{CS_TRANSCRIPT_SCHEMA_PATH};
        std::stringstream buffer;
        buffer << file.rdbuf();
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(nlohmann::json::parse(buffer.str()));
        return compiled;
    }();
    return validator;
}

std::vector<float> fiveSecondsMono() { return std::vector<float>(5000, 0.25f); }

TranscriptionOptions options() {
    return TranscriptionOptions{SourceId::create("cam-1").value(), "en"};
}

TEST(TranscriptPipelineTest, FakeOutputSerializesValidatesAndRoundTrips) {
    // AudioInput -> FakeTranscriptionProvider -> Transcript
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;
    const auto transcript = provider.transcribe(audio, options()).value();

    // -> serialize
    const nlohmann::json document = TranscriptSerializer::toJson(transcript);

    // -> validates against schemas/transcript.schema.json
    RecordingErrorHandler errors;
    transcriptValidator().validate(document, errors);
    ASSERT_FALSE(errors.failed())
        << "schema violation at '" << errors.pointer() << "': " << errors.message();

    // -> deserialize -> equals original
    const auto restored = TranscriptSerializer::fromJson(document);
    ASSERT_TRUE(restored.hasValue());
    EXPECT_TRUE(restored.value() == transcript);
}

TEST(TranscriptPipelineTest, SerializedDocumentIsSchemaVersioned) {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;
    const auto transcript = provider.transcribe(audio, options()).value();

    const nlohmann::json document = TranscriptSerializer::toJson(transcript);
    ASSERT_TRUE(document.contains("schemaVersion"));
    EXPECT_EQ(document["schemaVersion"].get<int>(), TranscriptSerializer::kSchemaVersion);
}

// ---- deserialization rejects corrupt documents via Result ----

nlohmann::json validDocument() {
    const auto samples = fiveSecondsMono();
    const auto audio = AudioInput::create(samples, 1000, 1).value();
    FakeTranscriptionProvider provider;
    return TranscriptSerializer::toJson(provider.transcribe(audio, options()).value());
}

TEST(TranscriptPipelineTest, RejectsUnknownSchemaVersion) {
    auto document = validDocument();
    document["schemaVersion"] = 999;
    const auto result = TranscriptSerializer::fromJson(document);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(TranscriptPipelineTest, RejectsNegativeStartNs) {
    auto document = validDocument();
    document["segments"][0]["timeRange"]["startNs"] = -1;
    EXPECT_FALSE(TranscriptSerializer::fromJson(document).hasValue());
}

TEST(TranscriptPipelineTest, RejectsNonMonotonicSegments) {
    auto document = validDocument();
    // Swap the two segments so they are out of order.
    auto s0 = document["segments"][0];
    document["segments"][0] = document["segments"][1];
    document["segments"][1] = s0;
    EXPECT_FALSE(TranscriptSerializer::fromJson(document).hasValue());
}

TEST(TranscriptPipelineTest, RejectsNonFiniteConfidence) {
    auto document = validDocument();
    // JSON cannot hold NaN as a number; a string is an equally invalid value and
    // must be rejected rather than silently coerced.
    document["segments"][0]["words"][0]["confidence"] = "oops";
    EXPECT_FALSE(TranscriptSerializer::fromJson(document).hasValue());
}

TEST(TranscriptPipelineTest, RejectsConfidenceAboveOne) {
    auto document = validDocument();
    document["segments"][0]["words"][0]["confidence"] = 1.5;
    EXPECT_FALSE(TranscriptSerializer::fromJson(document).hasValue());
}

TEST(TranscriptPipelineTest, RejectsNonObjectDocument) {
    EXPECT_FALSE(TranscriptSerializer::fromJson(nlohmann::json::array()).hasValue());
}

}  // namespace
