#include "whisper_adapter/WhisperTranscriptionProvider.h"

#include "transcription/AudioInput.h"
#include "transcription/Transcript.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

// Enabled-only (CS_ENABLE_WHISPER) real-inference test. It loads the upstream
// jfk.wav sample staged by the bootstrap and asserts a non-empty transcript with
// sane, in-bounds, monotonic project-timebase timestamps. It only runs in the
// whisper-enabled preset and is not required for the default build.

namespace {

namespace fs = std::filesystem;

using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::TranscriptionOptions;
using creator::whisper_adapter::WhisperProviderConfig;
using creator::whisper_adapter::WhisperTranscriptionProvider;

// Minimal PCM16 mono WAV reader: scans chunks for "data" and reads int16 -> float.
std::optional<std::vector<float>> readPcm16MonoWav(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 44) return std::nullopt;
    std::size_t offset = 12;  // past "RIFF"<size>"WAVE"
    while (offset + 8 <= bytes.size()) {
        const std::string id(bytes.data() + offset, 4);
        std::uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, bytes.data() + offset + 4, 4);
        const std::size_t body = offset + 8;
        if (id == "data") {
            std::vector<float> samples;
            const std::size_t count = std::min<std::size_t>(
                chunkSize, bytes.size() - body) / 2;
            samples.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                std::int16_t value = 0;
                std::memcpy(&value, bytes.data() + body + i * 2, 2);
                samples.push_back(static_cast<float>(value) / 32768.0f);
            }
            return samples;
        }
        offset = body + chunkSize + (chunkSize & 1u);
    }
    return std::nullopt;
}

WhisperProviderConfig config() {
    WhisperProviderConfig c;
    c.runtimeRoot = CS_TEST_WHISPER_ROOT;
    c.threadCount = 2;
    return c;
}

TEST(WhisperTranscriptionProviderTest, TranscribesKnownSampleWithSaneTimestamps) {
    const fs::path sample = CS_TEST_WHISPER_SAMPLE;
    if (!fs::exists(sample)) {
        GTEST_SKIP() << "jfk.wav sample not staged: " << sample;
    }
    auto pcm = readPcm16MonoWav(sample);
    ASSERT_TRUE(pcm.has_value());
    ASSERT_FALSE(pcm->empty());

    auto provider = WhisperTranscriptionProvider::create(config());
    ASSERT_TRUE(provider.hasValue()) << provider.error().message();

    const auto audio = AudioInput::create(*pcm, 16000, 1).value();
    const TranscriptionOptions options{SourceId::create("cam-1").value(), "en"};

    const auto result = provider.value()->transcribe(audio, options);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    const auto& transcript = result.value();

    ASSERT_FALSE(transcript.segments().empty());
    EXPECT_FALSE(transcript.fullText().empty());

    const std::int64_t audioNs = audio.duration().count();
    std::int64_t previousEnd = 0;
    for (const auto& segment : transcript.segments()) {
        const std::int64_t start = segment.range().start().time_since_epoch().count();
        const std::int64_t end = segment.range().end().time_since_epoch().count();
        EXPECT_GE(start, previousEnd);
        EXPECT_LT(start, end);
        // Allow a little slack past the exact duration for the final segment.
        EXPECT_LE(start, audioNs + 1'000'000'000);
        EXPECT_FALSE(segment.text().empty());
        for (const auto& word : segment.words()) {
            EXPECT_GE(word.confidence(), 0.0);
            EXPECT_LE(word.confidence(), 1.0);
        }
        previousEnd = end;
    }
}

TEST(WhisperTranscriptionProviderTest, RejectsEmptyAudio) {
    auto provider = WhisperTranscriptionProvider::create(config());
    ASSERT_TRUE(provider.hasValue()) << provider.error().message();

    const std::vector<float> empty;
    const auto audio = AudioInput::create(empty, 16000, 1).value();
    const TranscriptionOptions options{SourceId::create("cam-1").value(), "en"};

    const auto result = provider.value()->transcribe(audio, options);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
}

}  // namespace
