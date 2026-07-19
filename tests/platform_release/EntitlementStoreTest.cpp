#include "platform_release/EntitlementStore.h"
#include "platform_release/IReceiptVerifier.h"

#include "core/AppError.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::platform_release::EntitlementAssertion;
using creator::platform_release::EntitlementStateRecord;
using creator::platform_release::EntitlementStore;
using creator::platform_release::IReceiptVerifier;

class FakeReceiptVerifier final : public IReceiptVerifier {
public:
    [[nodiscard]] Result<EntitlementAssertion> verify(
        std::string_view providerId,
        std::span<const std::byte> opaqueReceipt) const override {
        ++calls;
        provider = std::string{providerId};
        bytes.assign(opaqueReceipt.begin(), opaqueReceipt.end());
        if (!accept) {
            return AppError{ErrorCode::InvalidArgument,
                            "provider receipt signature is invalid"};
        }
        return EntitlementAssertion{.productId = "creator-studio-pro",
                                    .validUntilUtcSeconds = 2'100'000,
                                    .revoked = false};
    }

    bool accept{true};
    mutable int calls{0};
    mutable std::string provider;
    mutable std::vector<std::byte> bytes;
};

class EntitlementStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "cs_entitlement_store_test";
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    EntitlementStore store() const { return EntitlementStore{root_ / "state.json"}; }

    EntitlementStateRecord record(std::int64_t lastOnline = 2'000'000,
                                  std::int64_t lastObserved = 2'000'100) const {
        return {.providerId = "provider-neutral-test",
                .assertion = {.productId = "creator-studio-pro",
                              .validUntilUtcSeconds = 2'100'000,
                              .revoked = false},
                .lastOnlineCheckUtcSeconds = lastOnline,
                .lastObservedUtcSeconds = lastObserved};
    }

    fs::path root_;
};

TEST_F(EntitlementStoreTest, RejectsEmptyProviderAndOversizedReceiptBeforeVerifier) {
    FakeReceiptVerifier verifier;
    const std::vector<std::byte> oneByte{std::byte{0x01}};
    EXPECT_FALSE(store().verifyReceipt("", oneByte, verifier).hasValue());
    const std::vector<std::byte> oversized((1024 * 1024) + 1, std::byte{0x01});
    EXPECT_FALSE(store().verifyReceipt("provider", oversized, verifier).hasValue());
    EXPECT_EQ(verifier.calls, 0);
}

TEST_F(EntitlementStoreTest, ReturnsOnlyProviderVerifiedAssertion) {
    FakeReceiptVerifier verifier;
    const std::vector<std::byte> receipt{std::byte{0x01}, std::byte{0x02}};
    const auto verified = store().verifyReceipt("provider", receipt, verifier);
    ASSERT_TRUE(verified.hasValue());
    EXPECT_EQ(verified.value().productId, "creator-studio-pro");
    EXPECT_EQ(verifier.provider, "provider");
    EXPECT_EQ(verifier.bytes, receipt);

    verifier.accept = false;
    const auto invalid = store().verifyReceipt("provider", receipt, verifier);
    ASSERT_FALSE(invalid.hasValue());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(EntitlementStoreTest, RejectsUnknownPersistedJsonFields) {
    std::ofstream{root_ / "state.json", std::ios::binary}
        << R"({"schemaVersion":1,"providerId":"provider","assertion":{"productId":"creator-studio-pro","validUntilUtcSeconds":2100000,"revoked":false},"lastOnlineCheckUtcSeconds":2000000,"lastObservedUtcSeconds":2000100,"receipt":"must-not-exist"})";
    const auto loaded = store().read();
    ASSERT_FALSE(loaded.hasValue());
    EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
}

TEST_F(EntitlementStoreTest, RejectsLastOnlineRollbackAndPreservesNewerState) {
    ASSERT_TRUE(store().write(record()).hasValue());
    const auto rollback = store().write(record(1'999'999, 2'000'200));
    ASSERT_FALSE(rollback.hasValue());
    EXPECT_EQ(rollback.error().code(), ErrorCode::InvalidState);
    const auto loaded = store().read();
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value(), record());
}

TEST_F(EntitlementStoreTest, IgnoresTornTemporaryReplacement) {
    ASSERT_TRUE(store().write(record()).hasValue());
    std::ofstream{root_ / ".state.json.part-torn", std::ios::binary}
        << R"({"schemaVersion":1)";

    const auto loaded = store().read();

    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value(), record());
}

TEST_F(EntitlementStoreTest, AtomicallyReplacesStateWithoutPersistingReceipt) {
    ASSERT_TRUE(store().write(record()).hasValue());
    const auto newer = record(2'000'200, 2'000'300);
    ASSERT_TRUE(store().write(newer).hasValue());
    const auto loaded = store().read();
    ASSERT_TRUE(loaded.hasValue());
    EXPECT_EQ(loaded.value(), newer);

    std::ifstream input{root_ / "state.json", std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(contents.find("receipt"), std::string::npos);
    for (const auto& entry : fs::directory_iterator(root_)) {
        EXPECT_EQ(entry.path().filename(), "state.json");
    }
}

}  // namespace
