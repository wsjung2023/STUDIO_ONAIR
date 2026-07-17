#include "avatar/AvatarMotionNdjsonSink.h"

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarProviderId.h"
#include "avatar/ExpressionParameters.h"
#include "core/AppError.h"
#include "core/Timebase.h"

#include <gtest/gtest.h>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::avatar::AvatarMotionNdjsonSink;
using creator::avatar::AvatarMotionSample;
using creator::avatar::AvatarProviderId;
using creator::avatar::ExpressionParameters;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

// Same collecting-error-handler shape AvatarMotionSerializerTest and
// project_store::ManifestSchemaValidator both use against their respective
// schemas: gather every violation rather than stopping at the first.
class EventSchemaCollectingErrorHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        failed_ = true;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool failed_{false};
};

const nlohmann::json_schema::json_validator& eventSchemaValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        std::ifstream schemaFile(CS_EVENT_SCHEMA_PATH);
        nlohmann::json schemaJson;
        schemaFile >> schemaJson;

        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(schemaJson);
        return compiled;
    }();
    return validator;
}

[[nodiscard]] bool validatesAgainstEventSchema(const nlohmann::json& document) {
    EventSchemaCollectingErrorHandler errors;
    eventSchemaValidator().validate(document, errors);
    return !errors.failed();
}

ExpressionParameters sampleParameters() {
    ExpressionParameters parameters{};
    parameters.eyeOpenLeft = 0.25F;
    parameters.eyeOpenRight = 0.5F;
    parameters.browUpLeft = 0.1F;
    parameters.browUpRight = 0.2F;
    parameters.mouthOpen = 0.75F;
    parameters.mouthWide = 0.6F;
    parameters.headYaw = -0.3F;
    parameters.headPitch = 0.4F;
    parameters.headRoll = -0.1F;
    return parameters;
}

AvatarMotionSample makeSample(std::int64_t timestampNs, std::string providerId = "fake-provider") {
    return AvatarMotionSample{
        .timestamp = TimestampNs{DurationNs{timestampNs}},
        .parameters = sampleParameters(),
        .provider = AvatarProviderId::create(std::move(providerId)).value(),
    };
}

/// Splits NDJSON text into its lines, dropping a single trailing empty
/// element caused by a final newline (matches how std::getline behaves).
std::vector<std::string> readLines(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

/// Creates a unique temp directory per test and removes it afterwards - even
/// when the test body fails an assertion, since gtest still runs TearDown()
/// for a controlled (non-crashing) test failure. Mirrors
/// project_store::JsonProjectStoreTest's fixture.
class AvatarMotionNdjsonSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        telemetryDir_ = fs::temp_directory_path() /
                       ("cs_test_" + std::string{info->test_suite_name()} + "_" +
                        std::string{info->name()});
        std::error_code ec;
        fs::remove_all(telemetryDir_, ec);
        fs::create_directories(telemetryDir_, ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(telemetryDir_, ec);
    }

    [[nodiscard]] fs::path ndjsonFile() const { return telemetryDir_ / AvatarMotionNdjsonSink::kFileName; }

    fs::path telemetryDir_;
};

TEST_F(AvatarMotionNdjsonSinkTest, AppendNSamplesWritesNSchemaValidLines) {
    AvatarMotionNdjsonSink sink{telemetryDir_};

    for (std::int64_t i = 0; i < 5; ++i) {
        const auto result = sink.append(makeSample(1'000 + i * 100));
        ASSERT_TRUE(result.hasValue()) << result.error().message();
    }

    const std::vector<std::string> lines = readLines(ndjsonFile());
    ASSERT_EQ(lines.size(), 5U);
    for (const std::string& line : lines) {
        const nlohmann::json parsed = nlohmann::json::parse(line);
        EXPECT_TRUE(validatesAgainstEventSchema(parsed)) << line;
    }
}

TEST_F(AvatarMotionNdjsonSinkTest, OrderingPreservedAndSecondAppendExtendsRatherThanTruncates) {
    AvatarMotionNdjsonSink sink{telemetryDir_};

    ASSERT_TRUE(sink.append(makeSample(10, "provider-a")).hasValue());
    ASSERT_TRUE(sink.append(makeSample(20, "provider-b")).hasValue());
    ASSERT_TRUE(sink.append(makeSample(30, "provider-c")).hasValue());

    const std::vector<std::string> lines = readLines(ndjsonFile());
    ASSERT_EQ(lines.size(), 3U);

    EXPECT_EQ(nlohmann::json::parse(lines[0])["tNs"].get<std::int64_t>(), 10);
    EXPECT_EQ(nlohmann::json::parse(lines[1])["tNs"].get<std::int64_t>(), 20);
    EXPECT_EQ(nlohmann::json::parse(lines[2])["tNs"].get<std::int64_t>(), 30);
    EXPECT_EQ(nlohmann::json::parse(lines[0])["provider"].get<std::string>(), "provider-a");
    EXPECT_EQ(nlohmann::json::parse(lines[1])["provider"].get<std::string>(), "provider-b");
    EXPECT_EQ(nlohmann::json::parse(lines[2])["provider"].get<std::string>(), "provider-c");
}

TEST_F(AvatarMotionNdjsonSinkTest, MissingParentDirectoryReturnsIoFailureWithNoThrow) {
    const fs::path missingParent = telemetryDir_ / "does" / "not" / "exist";
    AvatarMotionNdjsonSink sink{missingParent};

    EXPECT_NO_THROW({
        const auto result = sink.append(makeSample(1));
        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    });
}

TEST_F(AvatarMotionNdjsonSinkTest, DirectoryThatIsActuallyAFileReturnsIoFailureWithNoThrow) {
    const fs::path filePathUsedAsDirectory = telemetryDir_ / "not-a-directory";
    {
        std::ofstream placeholder{filePathUsedAsDirectory};
        placeholder << "not a directory";
    }
    AvatarMotionNdjsonSink sink{filePathUsedAsDirectory};

    EXPECT_NO_THROW({
        const auto result = sink.append(makeSample(1));
        ASSERT_FALSE(result.hasValue());
        EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    });
}

// The Task-9 regression class: a package path outside the process ANSI
// codepage (CP949 on this machine) must still round-trip. Built with the
// char8_t (u8"...") overload of fs::path's constructor, not L"...", for the
// same portability reason JsonProjectStoreTest.RoundTripsThroughNonAsciiPath
// documents: char8_t sources are defined by the C++20 standard to be UTF-8,
// converted to each platform's native encoding without going through any
// codepage or locale.
TEST_F(AvatarMotionNdjsonSinkTest, NonAsciiTelemetryDirectoryRoundTrips) {
    const fs::path unicodeDir = telemetryDir_ / fs::path{u8"텔레메트리_日本語_🎬"};
    std::error_code ec;
    fs::create_directories(unicodeDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    AvatarMotionNdjsonSink sink{unicodeDir};
    const auto result = sink.append(makeSample(42, "fake-provider"));
    ASSERT_TRUE(result.hasValue()) << result.error().message();

    const fs::path file = unicodeDir / AvatarMotionNdjsonSink::kFileName;
    ASSERT_TRUE(fs::exists(file));
    const std::vector<std::string> lines = readLines(file);
    ASSERT_EQ(lines.size(), 1U);
    const nlohmann::json parsed = nlohmann::json::parse(lines[0]);
    EXPECT_EQ(parsed["tNs"].get<std::int64_t>(), 42);
    EXPECT_TRUE(validatesAgainstEventSchema(parsed));

    fs::remove_all(unicodeDir, ec);
}

TEST_F(AvatarMotionNdjsonSinkTest, NegativeTimestampRejectedAndWritesNothing) {
    AvatarMotionNdjsonSink sink{telemetryDir_};

    const auto result = sink.append(makeSample(-1));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(ndjsonFile()))
        << "a negative-tNs sample must not create the NDJSON file at all";
}

TEST_F(AvatarMotionNdjsonSinkTest, NegativeTimestampAfterValidAppendsDoesNotExtendTheFile) {
    AvatarMotionNdjsonSink sink{telemetryDir_};
    ASSERT_TRUE(sink.append(makeSample(10)).hasValue());
    ASSERT_TRUE(sink.append(makeSample(20)).hasValue());

    const auto result = sink.append(makeSample(-5));

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    const std::vector<std::string> lines = readLines(ndjsonFile());
    EXPECT_EQ(lines.size(), 2U) << "a rejected negative-tNs sample must not extend the file";
}

// A NaN/Inf parameter field is in-range for the float type but serializes to
// JSON `null` under nlohmann's default dump(), which violates the
// avatar.motion schema's `parameters: additionalProperties {type: number}`.
// The sink is the write boundary, so it must reject this the same way it
// rejects a negative tNs: before any file touch, not after a schema-invalid
// line has already reached disk.
TEST_F(AvatarMotionNdjsonSinkTest, NonFiniteParameterRejectedAndWritesNothing) {
    AvatarMotionNdjsonSink sink{telemetryDir_};

    AvatarMotionSample sample = makeSample(10);
    sample.parameters.mouthOpen = std::numeric_limits<float>::quiet_NaN();

    const auto result = sink.append(sample);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(ndjsonFile()))
        << "a non-finite-parameter sample must not create the NDJSON file at all";
}

}  // namespace
