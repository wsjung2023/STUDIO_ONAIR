#include "core/Uuid.h"

#include <array>
#include <cstdint>
#include <random>

namespace creator::core {
namespace {

constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

std::uint64_t random64() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    static thread_local std::uniform_int_distribution<std::uint64_t> distribution;
    return distribution(engine);
}

[[nodiscard]] bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

std::string generateUuidV4() {
    std::array<std::uint8_t, 16> bytes{};
    const std::uint64_t high = random64();
    const std::uint64_t low = random64();
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>((high >> (8 * i)) & 0xFF);
        bytes[8 + i] = static_cast<std::uint8_t>((low >> (8 * i)) & 0xFF);
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant

    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHexDigits[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHexDigits[bytes[i] & 0x0F]);
    }
    return out;
}

bool isUuidV4(std::string_view text) noexcept {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool isSeparatorPosition = (i == 8 || i == 13 || i == 18 || i == 23);
        if (isSeparatorPosition) {
            if (text[i] != '-') {
                return false;
            }
        } else if (!isLowerHex(text[i])) {
            return false;
        }
    }
    if (text[14] != '4') {
        return false;
    }
    const char variant = text[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'b';
}

}  // namespace creator::core
