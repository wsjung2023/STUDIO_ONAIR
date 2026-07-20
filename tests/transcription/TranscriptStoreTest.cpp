#include "transcription/TranscriptStore.h"

#include "transcription/AudioInput.h"
#include "transcription/FakeTranscriptionProvider.h"
#include "transcription/Transcript.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using creator::core::ErrorCode;
using creator::domain::SourceId;
using creator::transcription::AudioInput;
using creator::transcription::FakeTranscriptionProvider;
using creator::transcription::Transcript;
using creator::transcription::TranscriptionOptions;
using creator::transcription::TranscriptStore;

// §8 resource-cleanup: a unique temp dir per test, removed on teardown whether
// the test passes or fails.
class TranscriptStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cs_transcription_" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->current_test_info()
                                                          ->line()) +
                "_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(dir_, ignored);
    }

    Transcript sampleTranscript() const {
        const std::vector<float> samples(5000, 0.25f);
        const auto audio = AudioInput::create(samples, 1000, 1).value();
        FakeTranscriptionProvider provider;
        return provider
            .transcribe(audio, TranscriptionOptions{SourceId::create("cam-1").value(), "en"})
            .value();
    }

    std::filesystem::path dir_;
    static inline int counter_ = 0;
};

TEST_F(TranscriptStoreTest, WriteThenReadRoundTrips) {
    TranscriptStore store{dir_};
    const auto transcript = sampleTranscript();

    const auto written = store.write("cam-1", transcript);
    ASSERT_TRUE(written.hasValue());
    EXPECT_TRUE(std::filesystem::exists(written.value()));

    const auto restored = store.read(written.value());
    ASSERT_TRUE(restored.hasValue());
    EXPECT_TRUE(restored.value() == transcript);
}

TEST_F(TranscriptStoreTest, WriteCreatesMissingDirectory) {
    const auto nested = dir_ / "transcripts";
    TranscriptStore store{nested};
    const auto written = store.write("cam-1", sampleTranscript());
    ASSERT_TRUE(written.hasValue());
    EXPECT_TRUE(std::filesystem::exists(nested));
}

TEST_F(TranscriptStoreTest, ReplacesExistingArtifactAtomically) {
    TranscriptStore store{dir_};
    ASSERT_TRUE(store.write("local-ai", sampleTranscript()).hasValue());

    const auto replaced = store.write("local-ai", sampleTranscript());

    ASSERT_TRUE(replaced.hasValue()) << replaced.error().message();
    EXPECT_TRUE(store.read(replaced.value()).hasValue());
    for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
        EXPECT_EQ(entry.path().filename().string().find(".part-"),
                  std::string::npos);
    }
}

TEST_F(TranscriptStoreTest, RejectsEmptyName) {
    TranscriptStore store{dir_};
    const auto result = store.write("", sampleTranscript());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(TranscriptStoreTest, RejectsNameWithPathSeparator) {
    TranscriptStore store{dir_};
    EXPECT_FALSE(store.write("a/b", sampleTranscript()).hasValue());
    EXPECT_FALSE(store.write("a\\b", sampleTranscript()).hasValue());
}

TEST_F(TranscriptStoreTest, ReadMissingFileReturnsNotFound) {
    TranscriptStore store{dir_};
    const auto result = store.read(dir_ / "does-not-exist.json");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST_F(TranscriptStoreTest, ReadCorruptFileReturnsParseFailure) {
    const auto path = dir_ / "corrupt.json";
    {
        std::ofstream out{path};
        out << "{ this is not json";
    }
    TranscriptStore store{dir_};
    const auto result = store.read(path);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::ParseFailure);
}

TEST_F(TranscriptStoreTest, FailedWriteLeavesNoPartialFile) {
    // The target directory path is occupied by a regular file, so the store
    // cannot create it: the write must fail and leave nothing behind.
    const auto blocker = dir_ / "blocked";
    {
        std::ofstream out{blocker};
        out << "x";
    }
    TranscriptStore store{blocker};  // not a directory
    const auto result = store.write("cam-1", sampleTranscript());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    // No transcript file was produced under the (non-)directory.
    EXPECT_FALSE(std::filesystem::exists(blocker / "cam-1.json"));
}

}  // namespace
