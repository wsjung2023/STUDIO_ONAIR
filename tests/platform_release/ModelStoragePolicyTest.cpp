#include "platform_release/ModelStoragePolicy.h"

#include "core/AppError.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>

namespace {

namespace fs = std::filesystem;
using creator::core::ErrorCode;
using creator::platform_release::ModelInstallRequest;
using creator::platform_release::ModelStoragePolicy;
using creator::platform_release::ModelStorageSnapshot;

constexpr auto kHash =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string hashOf(std::string_view text) {
    creator::core::Sha256 hash;
    hash.update(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    return hash.finish();
}

void writeText(const fs::path& path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(output.good());
}

class ModelStoragePolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("creator-studio-model-policy-" + creator::core::generateUuidV4());
        ASSERT_TRUE(fs::create_directories(root_ / "whisper"));
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] ModelInstallRequest request(
        std::uint64_t bytes = 128ULL * 1024ULL * 1024ULL) const {
        return {
            .finalPath = root_ / "whisper" / "base.bin",
            .downloadBytes = bytes,
            .expectedSha256 = kHash,
        };
    }

    fs::path root_;
};

TEST_F(ModelStoragePolicyTest, ProducesSameDirectoryAtomicStagingPlan) {
    const ModelStoragePolicy policy{root_, 512ULL * 1024ULL * 1024ULL};

    const auto plan = policy.admit(
        request(), ModelStorageSnapshot{.availableBytes = 1ULL << 30U});

    ASSERT_TRUE(plan.hasValue());
    EXPECT_EQ(plan.value().finalPath, root_ / "whisper" / "base.bin");
    EXPECT_EQ(plan.value().stagingPath,
              root_ / "whisper" / "base.bin.part");
    EXPECT_EQ(plan.value().stagingPath.parent_path(),
              plan.value().finalPath.parent_path());
    EXPECT_EQ(plan.value().expectedSha256, kHash);
}

TEST_F(ModelStoragePolicyTest, RejectsZeroByteAndOversizedModels) {
    const ModelStoragePolicy policy{root_, 256ULL * 1024ULL * 1024ULL};

    EXPECT_FALSE(policy.admit(
        request(0), ModelStorageSnapshot{.availableBytes = 1ULL << 30U})
                     .hasValue());
    const auto oversized = policy.admit(
        request(300ULL * 1024ULL * 1024ULL),
        ModelStorageSnapshot{.availableBytes = 1ULL << 30U});
    ASSERT_FALSE(oversized.hasValue());
    EXPECT_EQ(oversized.error().code(), ErrorCode::InsufficientStorage);
}

TEST_F(ModelStoragePolicyTest, RejectsInsufficientFreeOrAggregateBudget) {
    const ModelStoragePolicy policy{root_, 512ULL * 1024ULL * 1024ULL};

    EXPECT_FALSE(policy.admit(
        request(), ModelStorageSnapshot{.availableBytes = 64ULL * 1024ULL * 1024ULL})
                     .hasValue());
    EXPECT_FALSE(policy.admit(
        request(), ModelStorageSnapshot{
                       .availableBytes = 1ULL << 30U,
                       .installedModelBytes = 450ULL * 1024ULL * 1024ULL})
                     .hasValue());
}

TEST_F(ModelStoragePolicyTest, RejectsPathOutsideConfiguredRoot) {
    const ModelStoragePolicy policy{root_, 512ULL * 1024ULL * 1024ULL};
    auto outside = request();
    outside.finalPath = root_.parent_path() / "outside.bin";

    const auto plan = policy.admit(
        outside, ModelStorageSnapshot{.availableBytes = 1ULL << 30U});

    ASSERT_FALSE(plan.hasValue());
    EXPECT_EQ(plan.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ModelStoragePolicyTest, RejectsNonCanonicalSha256Text) {
    const ModelStoragePolicy policy{root_, 512ULL * 1024ULL * 1024ULL};
    const std::array<std::string, 3> invalidHashes{
        "abc", std::string(64, 'G'), std::string(65, '0')};
    for (const auto& hash : invalidHashes) {
        auto invalid = request();
        invalid.expectedSha256 = hash;
        EXPECT_FALSE(policy.admit(
            invalid, ModelStorageSnapshot{.availableBytes = 1ULL << 30U})
                         .hasValue());
    }
}

TEST_F(ModelStoragePolicyTest, PublishesVerifiedModelByAtomicReplacement) {
    const ModelStoragePolicy policy{root_, 1024};
    const std::string payload = "new-model";
    auto install = request(payload.size());
    install.expectedSha256 = hashOf(payload);
    const auto plan = policy.admit(
        install, ModelStorageSnapshot{.availableBytes = 1024});
    ASSERT_TRUE(plan.hasValue());
    writeText(plan.value().finalPath, "old-model");
    writeText(plan.value().stagingPath, payload);

    const auto published = policy.publishVerified(plan.value());

    ASSERT_TRUE(published.hasValue());
    EXPECT_FALSE(fs::exists(plan.value().stagingPath));
    std::ifstream input{plan.value().finalPath, std::ios::binary};
    const std::string publishedText{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(publishedText, payload);
}

TEST_F(ModelStoragePolicyTest, HashMismatchPreservesExistingModel) {
    const ModelStoragePolicy policy{root_, 1024};
    auto install = request(9);
    install.expectedSha256 = std::string(64, '0');
    const auto plan = policy.admit(
        install, ModelStorageSnapshot{.availableBytes = 1024});
    ASSERT_TRUE(plan.hasValue());
    writeText(plan.value().finalPath, "old-model");
    writeText(plan.value().stagingPath, "new-model");

    const auto published = policy.publishVerified(plan.value());

    ASSERT_FALSE(published.hasValue());
    EXPECT_EQ(published.error().code(), ErrorCode::IoFailure);
    std::ifstream input{plan.value().finalPath, std::ios::binary};
    const std::string publishedText{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(publishedText, "old-model");
    EXPECT_FALSE(fs::exists(plan.value().stagingPath));
}

}  // namespace
