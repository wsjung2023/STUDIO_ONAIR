#include "whisper_adapter/WhisperRuntimeManifest.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

// Enabled-only (CS_ENABLE_WHISPER) test. The ACCEPT case runs against the real
// audited prefix staged by scripts/bootstrap_whisper.ps1 (CS_TEST_WHISPER_ROOT);
// the REJECT cases use synthetic temp-dir manifests and all fail BEFORE any real
// model hash is needed, so they need no 77 MB fixture.

namespace {

namespace fs = std::filesystem;

constexpr const char* kVersion = "1.7.6";
constexpr const char* kCommit = "a8d002cfd879315632a579e73f0148d06959de36";
constexpr const char* kModelSha =
    "921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f";
constexpr const char* kDummySha =
    "0000000000000000000000000000000000000000000000000000000000000000";

class Fixture final {
public:
    Fixture() {
        root_ = fs::temp_directory_path() /
                ("creator-studio-whisper-manifest-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void writeFile(const std::string& relative, const std::string& content) {
        const auto path = root_ / fs::path{relative};
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    // filesJson is the raw "files":[...] array body.
    void writeManifest(const std::string& version, const std::string& modelSha,
                       const std::string& modelLicense,
                       const std::string& filesJson) {
        std::ofstream out(root_ / "whisper-runtime-manifest.json",
                          std::ios::binary | std::ios::trunc);
        out << "{\"abi\":1,\"component\":\"whisper.cpp\",\"version\":\"" << version
            << "\",\"source_commit\":\"" << kCommit
            << "\",\"linking\":\"dynamic\",\"model\":{\"name\":\"ggml-tiny.en.bin\""
               ",\"path\":\"share/whisper-models/ggml-tiny.en.bin\",\"sha256\":\""
            << modelSha << "\",\"license\":\"" << modelLicense
            << "\",\"source_url\":\"https://huggingface.co/ggerganov/whisper.cpp\"},"
               "\"files\":["
            << filesJson << "]}";
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

private:
    fs::path root_;
};

std::string fileEntry(const std::string& path, const std::string& sha,
                      const std::string& role, const std::string& license) {
    return "{\"path\":\"" + path + "\",\"sha256\":\"" + sha + "\",\"role\":\"" +
           role + "\",\"component\":\"whisper.cpp\",\"version\":\"1.7.6\","
           "\"source_identity\":\"" +
           std::string{kCommit} + "\",\"license\":\"" + license + "\"}";
}

TEST(WhisperRuntimeManifestTest, AcceptsAuditedRuntime) {
    const auto result =
        creator::whisper_adapter::verifyWhisperRuntimeManifest(CS_TEST_WHISPER_ROOT);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(fs::exists(result.value().modelPath));
    EXPECT_EQ(result.value().modelSha256, kModelSha);
}

TEST(WhisperRuntimeManifestTest, RejectsMissingManifest) {
    Fixture fixture;  // no manifest written
    const auto result =
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::NotFound);
}

TEST(WhisperRuntimeManifestTest, RejectsWrongVersion) {
    Fixture fixture;
    fixture.writeManifest("1.7.5", kModelSha, "MIT",
                          fileEntry("share/whisper-models/ggml-tiny.en.bin",
                                    kModelSha, "model", "MIT"));
    const auto result =
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(),
              creator::core::ErrorCode::UnsupportedVersion);
}

TEST(WhisperRuntimeManifestTest, RejectsWrongModelHashIdentity) {
    Fixture fixture;
    fixture.writeManifest(kVersion, kDummySha, "MIT",
                          fileEntry("share/whisper-models/ggml-tiny.en.bin",
                                    kDummySha, "model", "MIT"));
    EXPECT_FALSE(
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root())
            .hasValue());
}

TEST(WhisperRuntimeManifestTest, RejectsGplArtifact) {
    Fixture fixture;
    // A GPL entry placed first is rejected before any file hashing.
    fixture.writeManifest(kVersion, kModelSha, "MIT",
                          fileEntry("bin/tainted.dll", kDummySha,
                                    "runtime-library", "GPL-3.0-only"));
    EXPECT_FALSE(
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root())
            .hasValue());
}

TEST(WhisperRuntimeManifestTest, RejectsForbiddenExecutable) {
    Fixture fixture;
    fixture.writeManifest(kVersion, kModelSha, "MIT",
                          fileEntry("bin/helper.exe", kDummySha,
                                    "runtime-library", "MIT"));
    EXPECT_FALSE(
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root())
            .hasValue());
}

TEST(WhisperRuntimeManifestTest, RejectsPathTraversal) {
    Fixture fixture;
    fixture.writeManifest(kVersion, kModelSha, "MIT",
                          fileEntry("../escape.dll", kDummySha,
                                    "runtime-library", "MIT"));
    EXPECT_FALSE(
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root())
            .hasValue());
}

TEST(WhisperRuntimeManifestTest, RejectsMissingModelFileOnDisk) {
    Fixture fixture;
    // Identity is fine and the entry is MIT, but the model file is absent, so
    // hashing it fails.
    fixture.writeManifest(kVersion, kModelSha, "MIT",
                          fileEntry("share/whisper-models/ggml-tiny.en.bin",
                                    kModelSha, "model", "MIT"));
    EXPECT_FALSE(
        creator::whisper_adapter::verifyWhisperRuntimeManifest(fixture.root())
            .hasValue());
}

}  // namespace
