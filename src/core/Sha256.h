#pragma once

#include "core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace creator::core {

class Sha256 final {
public:
    void update(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::string finish();

private:
    void transform(const std::array<std::uint8_t, 64>& block);

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t totalBytes_{0};
    std::size_t buffered_{0};
};

[[nodiscard]] Result<std::string> sha256File(
    const std::filesystem::path& path);

}  // namespace creator::core
