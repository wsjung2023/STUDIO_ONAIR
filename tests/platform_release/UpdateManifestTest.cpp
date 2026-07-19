#include "platform_release/UpdateManifest.h"
#include "platform_release/UpdateManifestStore.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::platform_release::IUpdateSignatureVerifier;
using creator::platform_release::UpdateManifest;
using creator::platform_release::UpdateManifestStore;
using creator::platform_release::UpdateTarget;

constexpr const char* kShaA =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kShaB =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

std::vector<std::byte> signature() {
    return {std::byte{0x01}, std::byte{0x7f}, std::byte{0xa5}};
}

class RecordingVerifier final : public IUpdateSignatureVerifier {
public:
    [[nodiscard]] Result<void> verify(
        std::string_view canonicalPayload,
        std::span<const std::byte> detachedSignature) const override {
        ++calls;
        payload = std::string{canonicalPayload};
        observedSignature.assign(detachedSignature.begin(), detachedSignature.end());
        if (!accept) {
            return AppError{ErrorCode::InvalidArgument,
                            "detached update signature is invalid"};
        }
        return creator::core::ok();
    }

    bool accept{true};
    mutable int calls{0};
    mutable std::string payload;
    mutable std::vector<std::byte> observedSignature;
};

class UpdateManifestTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "cs_update_manifest_test";
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    UpdateManifest manifest(std::string version = "1.2.3") const {
        return UpdateManifest::create(
                   "stable", std::move(version),
                   {{"windows-x64", "packages/CreatorStudio.msix",
                     "https://updates.studioonair.example/stable/CreatorStudio.msix",
                     kShaA, 4096},
                    {"android-arm64", "packages/CreatorStudio.aab",
                     "https://updates.studioonair.example/stable/CreatorStudio.aab",
                     kShaB, 8192}})
            .value();
    }

    fs::path root_;
};

TEST_F(UpdateManifestTest, LoadsOnlyCanonicalSignedMetadata) {
    const auto expected = manifest();
    UpdateManifestStore store;
    const auto path = root_ / "stable.json";
    ASSERT_TRUE(store.write(path, expected, signature()).hasValue());

    RecordingVerifier verifier;
    const auto loaded = store.loadVerified(path, verifier);

    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    EXPECT_EQ(loaded.value(), expected);
    EXPECT_EQ(verifier.calls, 1);
    EXPECT_EQ(verifier.payload, expected.canonicalPayload());
    EXPECT_EQ(verifier.observedSignature, signature());
    EXPECT_EQ(loaded.value().targets().front().platform, "android-arm64");
}

TEST_F(UpdateManifestTest, RejectsUnknownPayloadAndEnvelopeFieldsBeforeSignatureVerification) {
    const auto path = root_ / "stable.json";
    std::ofstream{path, std::ios::binary}
        << R"({"payload":{"channel":"stable","productVersion":"1.2.3","schemaVersion":1,"targets":[],"telemetry":"on"},"signature":"017fa5","extra":true})";
    RecordingVerifier verifier;

    const auto loaded = UpdateManifestStore{}.loadVerified(path, verifier);

    EXPECT_FALSE(loaded.hasValue());
    EXPECT_EQ(verifier.calls, 0);
}

TEST_F(UpdateManifestTest, RejectsPathTraversalAndNonHttpsUrls) {
    EXPECT_FALSE(UpdateManifest::create(
                     "stable", "1.2.3",
                     {{"windows-x64", "../CreatorStudio.msix",
                       "https://updates.studioonair.example/app.msix", kShaA, 1}})
                     .hasValue());
    EXPECT_FALSE(UpdateManifest::create(
                     "stable", "1.2.3",
                     {{"windows-x64", "packages/CreatorStudio.msix",
                       "http://updates.studioonair.example/app.msix", kShaA, 1}})
                     .hasValue());
    EXPECT_FALSE(UpdateManifest::create(
                     "stable", "1.2.3",
                     {{"windows-x64", "packages/CreatorStudio.msix",
                       "https://updates.studioonair.example/../secret", kShaA, 1}})
                     .hasValue());
}

TEST_F(UpdateManifestTest, RejectsDuplicateTargetsAndMalformedSha256) {
    EXPECT_FALSE(UpdateManifest::create(
                     "stable", "1.2.3",
                     {{"windows-x64", "packages/a.msix",
                       "https://updates.studioonair.example/a.msix", kShaA, 1},
                      {"windows-x64", "packages/b.msix",
                       "https://updates.studioonair.example/b.msix", kShaB, 1}})
                     .hasValue());
    EXPECT_FALSE(UpdateManifest::create(
                     "stable", "1.2.3",
                     {{"windows-x64", "packages/a.msix",
                       "https://updates.studioonair.example/a.msix", "ABC", 1}})
                     .hasValue());
}

TEST_F(UpdateManifestTest, RejectsInvalidDetachedSignature) {
    const auto path = root_ / "stable.json";
    ASSERT_TRUE(UpdateManifestStore{}.write(path, manifest(), signature()).hasValue());
    RecordingVerifier verifier;
    verifier.accept = false;

    const auto loaded = UpdateManifestStore{}.loadVerified(path, verifier);

    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(verifier.calls, 1);
    EXPECT_EQ(loaded.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(UpdateManifestTest, AtomicallyReplacesAnExistingManifest) {
    const auto path = root_ / "stable.json";
    UpdateManifestStore store;
    ASSERT_TRUE(store.write(path, manifest("1.2.3"), signature()).hasValue());
    ASSERT_TRUE(store.write(path, manifest("1.2.4"), signature()).hasValue());

    RecordingVerifier verifier;
    const auto loaded = store.loadVerified(path, verifier);
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value().productVersion(), "1.2.4");

    for (const auto& entry : fs::directory_iterator(root_)) {
        EXPECT_EQ(entry.path().extension(), ".json");
    }
}

}  // namespace
