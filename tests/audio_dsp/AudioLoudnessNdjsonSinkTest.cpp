#include "audio_dsp/AudioLoudnessNdjsonSink.h"

#include "audio_dsp/AudioLoudnessSample.h"
#include "audio_dsp/LoudnessMeter.h"
#include "core/AppError.h"
#include "core/Timebase.h"
#include "support/EventSchemaValidator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using creator::audio_dsp::AudioLoudnessNdjsonSink;
using creator::audio_dsp::AudioLoudnessSample;
using creator::audio_dsp::LoudnessMeter;
using creator::audio_dsp::test::validatesAgainstEventSchema;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TimestampNs atNs(std::int64_t ns) {
    return TimestampNs{std::chrono::nanoseconds{ns}};
}

AudioLoudnessSample sampleAt(std::int64_t ns, double integrated = -23.0) {
    AudioLoudnessSample sample;
    sample.tNs = atNs(ns);
    sample.integratedLufs = integrated;
    sample.momentaryLufs = -21.0;
    sample.shortTermLufs = -22.0;
    sample.truePeakDbtp = -1.0;
    sample.sourceId = "mic-1";
    return sample;
}

// Temp-dir fixture: each test writes into an isolated directory that is removed
// in TearDown (CLAUDE.md 8 — resource cleanup verified).
class AudioLoudnessNdjsonSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = std::filesystem::temp_directory_path() /
               ("cs_loudness_ndjson_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" +
                std::to_string(stamp));
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        EXPECT_FALSE(std::filesystem::exists(dir_)) << "temp dir not cleaned up";
    }

    std::filesystem::path file() const { return dir_ / "loudness.ndjson"; }

    static std::vector<std::string> readLines(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    std::filesystem::path dir_;
};

TEST_F(AudioLoudnessNdjsonSinkTest, AppendedLinesAllValidateAgainstSchema) {
    constexpr int kCount = 5;
    {
        auto sink = AudioLoudnessNdjsonSink::open(file());
        ASSERT_TRUE(sink.hasValue());
        for (int i = 0; i < kCount; ++i) {
            ASSERT_TRUE(sink.value().append(sampleAt(i * 100'000'000)).hasValue());
        }
    }  // sink destroyed here -> file closed (RAII)

    const auto lines = readLines(file());
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(kCount));
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto event = nlohmann::json::parse(lines[i]);  // independently parseable
        std::string whyNot;
        EXPECT_TRUE(validatesAgainstEventSchema(event, &whyNot))
            << "line " << i << ": " << whyNot;
        EXPECT_EQ(event.at("type"), "audio.loudness");
        // Append order is preserved.
        EXPECT_EQ(event.at("tNs"), static_cast<std::int64_t>(i) * 100'000'000);
    }
}

TEST_F(AudioLoudnessNdjsonSinkTest, NullMeasurementLinesValidate) {
    {
        auto sink = AudioLoudnessNdjsonSink::open(file());
        ASSERT_TRUE(sink.hasValue());
        AudioLoudnessSample sample = sampleAt(0);
        sample.integratedLufs = LoudnessMeter::kNoMeasurement;
        sample.momentaryLufs = LoudnessMeter::kNoMeasurement;
        ASSERT_TRUE(sink.value().append(sample).hasValue());
    }

    const auto lines = readLines(file());
    ASSERT_EQ(lines.size(), 1U);
    const auto event = nlohmann::json::parse(lines[0]);
    std::string whyNot;
    EXPECT_TRUE(validatesAgainstEventSchema(event, &whyNot)) << whyNot;
    EXPECT_TRUE(event.at("integratedLufs").is_null());
    EXPECT_TRUE(event.at("momentaryLufs").is_null());
}

TEST_F(AudioLoudnessNdjsonSinkTest, ReopenAppendsWithoutTruncatingAndNoPartialLine) {
    {
        auto sink = AudioLoudnessNdjsonSink::open(file());
        ASSERT_TRUE(sink.hasValue());
        ASSERT_TRUE(sink.value().append(sampleAt(0)).hasValue());
    }
    {
        auto sink = AudioLoudnessNdjsonSink::open(file());  // reopen, append mode
        ASSERT_TRUE(sink.hasValue());
        ASSERT_TRUE(sink.value().append(sampleAt(200'000'000)).hasValue());
    }

    // Well-formed file: exactly two whole lines, each independently valid, and
    // the raw bytes end with a newline (no dangling partial line).
    const auto lines = readLines(file());
    ASSERT_EQ(lines.size(), 2U);
    for (const auto& line : lines) {
        const auto event = nlohmann::json::parse(line);
        std::string whyNot;
        EXPECT_TRUE(validatesAgainstEventSchema(event, &whyNot)) << whyNot;
    }
    std::ifstream in(file(), std::ios::binary);
    std::ostringstream raw;
    raw << in.rdbuf();
    const std::string bytes = raw.str();
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes.back(), '\n') << "file must not end mid-line";
}

TEST_F(AudioLoudnessNdjsonSinkTest, UnserializableSampleReturnsErrorAndWritesNoLine) {
    auto sink = AudioLoudnessNdjsonSink::open(file());
    ASSERT_TRUE(sink.hasValue());

    AudioLoudnessSample bad = sampleAt(-1);  // negative tNs cannot serialize
    const auto result = sink.value().append(bad);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);

    // A good sample after the rejected one is the only line written.
    ASSERT_TRUE(sink.value().append(sampleAt(300'000'000)).hasValue());

    const auto lines = readLines(file());
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_EQ(nlohmann::json::parse(lines[0]).at("tNs"), 300'000'000);
}

}  // namespace
