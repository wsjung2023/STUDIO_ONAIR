#include "rnnoise_adapter/RnnoiseRuntimeManifest.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

// SHA-256 of the ASCII string "hello".
constexpr const char* kHelloSha256 =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

constexpr const char* kVersion = "0.1.1";
constexpr const char* kCommit = "6cbfd53eb348a8d394e0757b4025c6ded34eb2b6";
constexpr const char* kArchiveSha =
    "1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987";

struct Entry final {
    std::string path;
    std::string hash{kHelloSha256};
    std::string role{"development"};
    std::string component{"RNNoise"};
    std::string version{kVersion};
    std::string sourceIdentity{kCommit};
    std::string license{"BSD-3-Clause"};
};

class RuntimeFixture final {
public:
    RuntimeFixture() {
        root_ = fs::temp_directory_path() /
                ("creator-studio-rnnoise-manifest-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_);
        entries_ = {
            {"lib/rnnoise.lib", kHelloSha256, "development"},
            {"include/rnnoise.h", kHelloSha256, "development"},
            {"creator-studio-rnnoise-build.txt", kHelloSha256, "evidence",
             "Creator Studio", "1", "repository:R2-audio-dsp",
             "LicenseRef-Creator-Studio-Proprietary"},
        };
        for (const auto& entry : entries_) writeFile(entry.path, "hello");
        writeManifest();
    }
    ~RuntimeFixture() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    void writeFile(const std::string& relative, const std::string& content) {
        const auto path = root_ / fs::path{relative};
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    void writeManifest(std::string version = kVersion,
                       std::string linking = "static") {
        std::ofstream output(root_ / "rnnoise-runtime-manifest.json",
                             std::ios::binary | std::ios::trunc);
        output << "{\"abi\":1,\"component\":\"RNNoise\",\"version\":\"" << version
               << "\",\"source_commit\":\"" << kCommit
               << "\",\"source_archive_sha256\":\"" << kArchiveSha
               << "\",\"linking\":\"" << linking
               << "\",\"license\":\"BSD-3-Clause\",\"files\":[";
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (index != 0) output << ',';
            const auto& entry = entries_[index];
            output << "{\"path\":\"" << entry.path << "\",\"sha256\":\""
                   << entry.hash << "\",\"role\":\"" << entry.role
                   << "\",\"component\":\"" << entry.component
                   << "\",\"version\":\"" << entry.version
                   << "\",\"source_identity\":\"" << entry.sourceIdentity
                   << "\",\"license\":\"" << entry.license << "\"}";
        }
        output << "]}";
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    [[nodiscard]] std::vector<Entry>& entries() noexcept { return entries_; }

private:
    fs::path root_;
    std::vector<Entry> entries_;
};

using creator::rnnoise_adapter::verifyRnnoiseRuntimeManifest;

TEST(RnnoiseRuntimeManifestTest, AcceptsExactAuditedFileSetAndHashes) {
    RuntimeFixture fixture;
    const auto result = verifyRnnoiseRuntimeManifest(fixture.root());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
}

TEST(RnnoiseRuntimeManifestTest, RejectsChangedHash) {
    RuntimeFixture fixture;
    fixture.writeFile("lib/rnnoise.lib", "tampered");
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(fixture.root()).hasValue());
}

TEST(RnnoiseRuntimeManifestTest, RejectsFalsePerFileProvenance) {
    RuntimeFixture fixture;
    fixture.entries().front().license = "GPL-3.0-only";
    fixture.writeManifest();
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(fixture.root()).hasValue());
}

TEST(RnnoiseRuntimeManifestTest, RejectsMissingRequiredArtifacts) {
    RuntimeFixture missing;
    fs::remove(missing.root() / "lib/rnnoise.lib");
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(missing.root()).hasValue());
}

TEST(RnnoiseRuntimeManifestTest, RejectsUnexpectedFile) {
    RuntimeFixture unexpected;
    unexpected.writeFile("lib/extra.txt", "hello");
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(unexpected.root()).hasValue());
}

TEST(RnnoiseRuntimeManifestTest, RejectsWrongVersionAndLinking) {
    RuntimeFixture version;
    version.writeManifest("0.1.0");
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(version.root()).hasValue());

    RuntimeFixture linking;
    linking.writeManifest(kVersion, "dynamic");
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(linking.root()).hasValue());
}

TEST(RnnoiseRuntimeManifestTest, RejectsTraversalDuplicateAndForbidden) {
    RuntimeFixture traversal;
    traversal.entries().front().path = "../rnnoise.lib";
    traversal.writeManifest();
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(traversal.root()).hasValue());

    RuntimeFixture duplicate;
    duplicate.entries().push_back(duplicate.entries().front());
    duplicate.writeManifest();
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(duplicate.root()).hasValue());

    RuntimeFixture forbidden;
    forbidden.entries().push_back(
        {"lib/rnnoise.dll", kHelloSha256, "development"});
    forbidden.writeFile("lib/rnnoise.dll", "hello");
    forbidden.writeManifest();
    EXPECT_FALSE(verifyRnnoiseRuntimeManifest(forbidden.root()).hasValue());
}

}  // namespace
