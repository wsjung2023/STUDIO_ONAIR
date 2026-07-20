#pragma once

#include "core/Result.h"
#include "core/Timebase.h"

#include <cstdint>
#include <optional>

namespace creator::app::android {

enum class AndroidMediaCodecKind { Video, Audio };

struct AndroidMediaCodecFormat final {
    AndroidMediaCodecKind kind{AndroidMediaCodecKind::Video};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t sampleRate{};
    std::uint32_t channels{};

    [[nodiscard]] static AndroidMediaCodecFormat video(
        std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] static AndroidMediaCodecFormat audio(
        std::uint32_t sampleRate, std::uint32_t channels) noexcept;

    friend bool operator==(const AndroidMediaCodecFormat&,
                           const AndroidMediaCodecFormat&) = default;
};

/// Generation and format gate for one reusable synchronous MediaCodec adapter.
/// Java owns native codec handles; this class prevents late work from an old
/// segment from reaching a newer handle and enforces monotonic project time.
class AndroidMediaCodecSession final {
public:
    [[nodiscard]] core::Result<std::uint64_t> begin(
        core::TimestampNs startTime);
    [[nodiscard]] core::Result<void> accept(
        std::uint64_t generation, core::TimestampNs timestamp,
        AndroidMediaCodecFormat format);
    [[nodiscard]] core::Result<void> finish(
        std::uint64_t generation, core::TimestampNs endTime);
    void abort(std::uint64_t generation) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

private:
    std::uint64_t generation_{};
    core::TimestampNs startTime_{};
    std::optional<core::TimestampNs> lastTimestamp_;
    std::optional<AndroidMediaCodecFormat> format_;
    bool active_{};
};

}  // namespace creator::app::android
