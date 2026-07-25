#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using creator::avatar::AvatarParameterValue;
using creator::avatar::inochi2d::Inochi2dAvatarRenderer;
using creator::core::ErrorCode;
using creator::core::TimestampNs;

TEST(Inochi2dAvatarRendererTest, ReportsMissingRuntimeAsInvalidState) {
    Inochi2dAvatarRenderer renderer{nullptr, 4, 4};
    const auto result = renderer.render(TimestampNs{}, std::vector<AvatarParameterValue>{});
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

#if defined(CS_INOCHI2D_ACTUAL_ROOT)

// Reads an environment variable without tripping MSVC's C4996 on std::getenv.
[[nodiscard]] std::string readEnv(const char* name) {
#ifdef _WIN32
    char buffer[2048] = {0};
    std::size_t length = 0;
    if (getenv_s(&length, buffer, sizeof buffer, name) != 0) return {};
    return std::string{buffer};
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

// End-to-end proof that the audited runtime loads a real .inx puppet and the
// software rasteriser draws it. Point CS_INOCHI2D_SAMPLE_MODEL at a real .inx to
// run; set CS_INOCHI2D_RENDER_OUT to dump tightly packed RGBA for visual review.
TEST(Inochi2dAvatarRendererTest, RendersRealPuppetToVisiblePixels) {
    const std::string modelPath = readEnv("CS_INOCHI2D_SAMPLE_MODEL");
    if (modelPath.empty() || !std::filesystem::exists(modelPath)) {
        GTEST_SKIP() << "Set CS_INOCHI2D_SAMPLE_MODEL to a real .inx puppet to run.";
    }
    constexpr std::uint32_t kWidth = 512;
    constexpr std::uint32_t kHeight = 512;
    auto renderer = Inochi2dAvatarRenderer::open(
        CS_INOCHI2D_ACTUAL_ROOT, std::filesystem::path{modelPath}, kWidth, kHeight);
    ASSERT_TRUE(renderer.hasValue()) << renderer.error().message();

    auto frame = renderer.value()->render(TimestampNs{}, {});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    EXPECT_EQ(frame.value().width(), kWidth);
    EXPECT_EQ(frame.value().height(), kHeight);

    const auto bytes = frame.value().bytes();
    const std::uint32_t stride = frame.value().stride();
    ASSERT_FALSE(bytes.empty());

    std::uint64_t opaque = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::size_t alpha =
                static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4 + 3;
            if (alpha < bytes.size() && bytes[alpha] != 0) ++opaque;
        }
    }
    EXPECT_GT(opaque, 0U) << "the rendered puppet frame is fully transparent";

    const auto dumpRgba = [&](const creator::avatar::AvatarRenderFrame& f,
                              const std::string& path) {
        if (path.empty()) return;
        const auto data = f.bytes();
        const std::uint32_t rowStride = f.stride();
        std::vector<std::uint8_t> rgba;
        rgba.reserve(static_cast<std::size_t>(kWidth) * kHeight * 4);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                const std::size_t p = static_cast<std::size_t>(y) * rowStride +
                                      static_cast<std::size_t>(x) * 4;
                rgba.push_back(data[p + 2]);  // R
                rgba.push_back(data[p + 1]);  // G
                rgba.push_back(data[p + 0]);  // B
                rgba.push_back(data[p + 3]);  // A
            }
        }
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rgba.data()),
                  static_cast<std::streamsize>(rgba.size()));
    };
    dumpRgba(frame.value(), readEnv("CS_INOCHI2D_RENDER_OUT"));

    // Idle motion: a frame rendered ~0.83 s later must differ from the first --
    // the puppet gently breathes/sways so it is not a frozen picture.
    auto laterFrame = renderer.value()->render(
        TimestampNs{creator::core::DurationNs{830'000'000}}, {});
    ASSERT_TRUE(laterFrame.hasValue()) << laterFrame.error().message();
    const auto laterBytes = laterFrame.value().bytes();
    EXPECT_FALSE(std::equal(bytes.begin(), bytes.end(), laterBytes.begin(),
                            laterBytes.end()))
        << "idle motion produced an identical frame 0.83 s later";
    dumpRgba(laterFrame.value(), readEnv("CS_INOCHI2D_RENDER_OUT2"));

    // Parameter-driven expression: setting real rig parameters (eyes closed, jaw
    // open) drives a rigged puppet -- this is the tracking path. A static puppet
    // exposes none of these, so they are skipped and it simply holds its pose.
    const std::vector<AvatarParameterValue> expression{
        {"Eye:: Left:: Open", 0.0F},
        {"Eye:: Right:: Open", 0.0F},
        {"Mouth:: Jaw Open", 1.0F},
    };
    auto expressionFrame = renderer.value()->render(
        TimestampNs{creator::core::DurationNs{1'000'000'000}}, expression);
    ASSERT_TRUE(expressionFrame.hasValue()) << expressionFrame.error().message();
    dumpRgba(expressionFrame.value(), readEnv("CS_INOCHI2D_RENDER_OUT3"));
}

// Isolates parameter-driven expression from idle motion. At timestamp 0 the idle
// breath/sway is exactly zero (sin(0)), so two independent puppets rendered at
// the same timestamp differ ONLY by the parameters applied to one of them. This
// proves the tracking channels actually deform the rigged face -- not merely that
// a frame renders -- which the confounded base-vs-expression diff above cannot.
TEST(Inochi2dAvatarRendererTest, ExpressionParametersChangeThePuppetAtRest) {
    const std::string modelPath = readEnv("CS_INOCHI2D_SAMPLE_MODEL");
    if (modelPath.empty() || !std::filesystem::exists(modelPath)) {
        GTEST_SKIP() << "Set CS_INOCHI2D_SAMPLE_MODEL to a rigged .inp/.inx puppet to run.";
    }
    constexpr std::uint32_t kWidth = 512;
    constexpr std::uint32_t kHeight = 512;
    auto neutral = Inochi2dAvatarRenderer::open(
        CS_INOCHI2D_ACTUAL_ROOT, std::filesystem::path{modelPath}, kWidth, kHeight);
    ASSERT_TRUE(neutral.hasValue()) << neutral.error().message();
    auto expressive = Inochi2dAvatarRenderer::open(
        CS_INOCHI2D_ACTUAL_ROOT, std::filesystem::path{modelPath}, kWidth, kHeight);
    ASSERT_TRUE(expressive.hasValue()) << expressive.error().message();

    auto neutralFrame = neutral.value()->render(TimestampNs{}, {});
    ASSERT_TRUE(neutralFrame.hasValue()) << neutralFrame.error().message();

    // Names taken from the real rig: eyes closed, jaw open.
    const std::vector<AvatarParameterValue> expression{
        {"Eye:: Left:: Open", 0.0F},
        {"Eye:: Right:: Open", 0.0F},
        {"Mouth:: Jaw Open", 1.0F},
    };
    auto expressiveFrame = expressive.value()->render(TimestampNs{}, expression);
    ASSERT_TRUE(expressiveFrame.hasValue()) << expressiveFrame.error().message();

    const auto neutralBytes = neutralFrame.value().bytes();
    const auto expressiveBytes = expressiveFrame.value().bytes();
    ASSERT_EQ(neutralBytes.size(), expressiveBytes.size());
    std::uint64_t differing = 0;
    for (std::size_t i = 0; i + 3 < neutralBytes.size(); i += 4) {
        if (neutralBytes[i] != expressiveBytes[i] ||
            neutralBytes[i + 1] != expressiveBytes[i + 1] ||
            neutralBytes[i + 2] != expressiveBytes[i + 2] ||
            neutralBytes[i + 3] != expressiveBytes[i + 3]) {
            ++differing;
        }
    }
    // At rest the only variable is the parameters, so any material pixel change is
    // attributable to them. Eyes closing plus an open jaw moves far more than this.
    EXPECT_GT(differing, static_cast<std::uint64_t>(kWidth) * kHeight / 1000)
        << "expression parameters did not change the rendered puppet at rest";
}

#endif  // CS_INOCHI2D_ACTUAL_ROOT

}  // namespace
