#include "avatar/vrm/GlbContainer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using creator::avatar::vrm::GlbContainer;
using creator::core::ErrorCode;

void appendU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFFU));
}

// Builds a well-formed glb from a JSON string and an optional BIN payload,
// with the spec's 4-byte chunk alignment (JSON padded with spaces, BIN with 0).
std::vector<std::byte> makeGlb(const std::string& json,
                               const std::vector<std::byte>& bin) {
    std::vector<std::byte> jsonChunk(json.size());
    std::memcpy(jsonChunk.data(), json.data(), json.size());
    while (jsonChunk.size() % 4U != 0U)
        jsonChunk.push_back(static_cast<std::byte>(0x20));  // space pad
    std::vector<std::byte> binChunk = bin;
    while (binChunk.size() % 4U != 0U)
        binChunk.push_back(static_cast<std::byte>(0x00));

    std::vector<std::byte> body;
    appendU32(body, static_cast<std::uint32_t>(jsonChunk.size()));
    appendU32(body, 0x4E4F534AU);  // JSON
    body.insert(body.end(), jsonChunk.begin(), jsonChunk.end());
    if (!bin.empty()) {
        appendU32(body, static_cast<std::uint32_t>(binChunk.size()));
        appendU32(body, 0x004E4942U);  // BIN
        body.insert(body.end(), binChunk.begin(), binChunk.end());
    }

    std::vector<std::byte> glb;
    appendU32(glb, 0x46546C67U);  // magic "glTF"
    appendU32(glb, 2U);           // version
    appendU32(glb, static_cast<std::uint32_t>(12U + body.size()));
    glb.insert(glb.end(), body.begin(), body.end());
    return glb;
}

TEST(GlbContainerTest, ReadsJsonAndBinChunks) {
    const std::string json = R"({"asset":{"version":"2.0"}})";
    const std::vector<std::byte> bin{std::byte{1}, std::byte{2}, std::byte{3}};
    const auto glb = makeGlb(json, bin);

    const auto result = GlbContainer::read(glb);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().json, json);
    ASSERT_EQ(result.value().bin.size(), 4U);  // padded to 4
    EXPECT_EQ(result.value().bin[0], std::byte{1});
    EXPECT_EQ(result.value().bin[2], std::byte{3});
}

TEST(GlbContainerTest, ReadsJsonOnlyContainer) {
    const auto glb = makeGlb(R"({"asset":{"version":"2.0"}})", {});
    const auto result = GlbContainer::read(glb);
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(result.value().bin.empty());
}

TEST(GlbContainerTest, RejectsBadMagicVersionAndTruncation) {
    auto glb = makeGlb(R"({"a":1})", {});
    glb[0] = std::byte{0};  // corrupt magic
    EXPECT_EQ(GlbContainer::read(glb).error().code(), ErrorCode::ParseFailure);

    auto version = makeGlb(R"({"a":1})", {});
    version[4] = std::byte{1};  // version 1
    EXPECT_EQ(GlbContainer::read(version).error().code(),
              ErrorCode::UnsupportedVersion);

    const auto full = makeGlb(R"({"asset":{"version":"2.0"}})", {});
    const std::vector<std::byte> truncated(full.begin(), full.begin() + 10);
    EXPECT_EQ(GlbContainer::read(truncated).error().code(),
              ErrorCode::ParseFailure);
}

TEST(GlbContainerTest, RejectsChunkLengthPastContainer) {
    auto glb = makeGlb(R"({"asset":{"version":"2.0"}})", {});
    // Inflate the JSON chunk length (bytes 12..16) far past the file.
    glb[12] = std::byte{0xFF};
    glb[13] = std::byte{0xFF};
    EXPECT_EQ(GlbContainer::read(glb).error().code(), ErrorCode::ParseFailure);
}

}  // namespace
