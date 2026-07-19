#include "platform_release/ReleaseManifest.h"
#include "platform_release/ReleaseManifestStore.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using creator::core::ErrorCode;
using creator::platform_release::ReleaseArtifact;
using creator::platform_release::ReleaseManifest;
using creator::platform_release::ReleaseManifestStore;

class ReleaseManifestTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "cs_release_manifest_test";
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
        fs::create_directories(dir_ / "bin");
        std::ofstream{dir_ / "bin" / "CreatorStudio.exe", std::ios::binary} << "release bytes";
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }
    ReleaseManifest manifest() const {
        const auto hash = creator::core::sha256File(dir_ / "bin" / "CreatorStudio.exe").value();
        return ReleaseManifest::create("1.0.0", "abc1234", "windows-x64",
                                       {{"bin/CreatorStudio.exe", hash}}).value();
    }
    fs::path dir_;
};

TEST_F(ReleaseManifestTest, RejectsInvalidArtifactPathAndHash) {
    EXPECT_FALSE(ReleaseManifest::create("1.0.0", "abc1234", "windows-x64",
                                         {{"../app.exe", "00"}}).hasValue());
}

TEST_F(ReleaseManifestTest, StoresArtifactsInCanonicalPathOrder) {
    const auto hash = creator::core::sha256File(dir_ / "bin" / "CreatorStudio.exe").value();
    const auto manifest = ReleaseManifest::create(
        "1.0.0", "abc1234", "windows-x64",
        {{"z.txt", hash}, {"bin/CreatorStudio.exe", hash}});
    ASSERT_TRUE(manifest.hasValue());
    EXPECT_EQ(manifest.value().artifacts().front().relativePath, "bin/CreatorStudio.exe");
}

TEST_F(ReleaseManifestTest, WritesThenReadsOnlyVerifiedArtifacts) {
    ReleaseManifestStore store;
    const auto path = dir_ / "release-manifest.json";
    ASSERT_TRUE(store.write(path, dir_, manifest()).hasValue());
    const auto restored = store.read(path, dir_);
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(restored.value(), manifest());
}

TEST_F(ReleaseManifestTest, RejectsArtifactChangedAfterManifestWrite) {
    ReleaseManifestStore store;
    const auto path = dir_ / "release-manifest.json";
    ASSERT_TRUE(store.write(path, dir_, manifest()).hasValue());
    std::ofstream{dir_ / "bin" / "CreatorStudio.exe", std::ios::binary | std::ios::trunc} << "changed";
    const auto restored = store.read(path, dir_);
    ASSERT_FALSE(restored.hasValue());
    EXPECT_EQ(restored.error().code(), ErrorCode::IoFailure);
}

}  // namespace
