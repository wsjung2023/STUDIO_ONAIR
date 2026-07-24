#include "avatar_pack_adapter/CatalogPathPolicy.h"

#include <algorithm>
#include <array>

namespace creator::avatar_pack_adapter::detail {
namespace {

constexpr std::size_t kMaximumComponentBytes = 128U;

bool asciiLowerAlpha(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

bool asciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool asciiAlphaNumeric(char value) noexcept {
    return asciiLowerAlpha(value) || asciiDigit(value);
}

bool reservedWindowsBasename(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 22U> reserved{
        "con",  "prn",  "aux",  "nul",  "com1", "com2",
        "com3", "com4", "com5", "com6", "com7", "com8",
        "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
        "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::find(reserved.begin(), reserved.end(), value) !=
           reserved.end();
}

bool numericIdentifier(std::string_view value) noexcept {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), asciiDigit) &&
           (value.size() == 1U || value.front() != '0');
}

bool validSemVerIdentifiers(std::string_view value,
                            bool rejectNumericLeadingZero) noexcept {
    if (value.empty()) return false;
    std::size_t begin = 0U;
    for (;;) {
        const auto dot = value.find('.', begin);
        const auto identifier =
            value.substr(begin, dot == std::string_view::npos
                                    ? std::string_view::npos
                                    : dot - begin);
        if (identifier.empty() ||
            !std::all_of(identifier.begin(), identifier.end(),
                         [](char character) {
                             return asciiAlphaNumeric(character) ||
                                    character == '-';
                         }) ||
            (rejectNumericLeadingZero &&
             std::all_of(identifier.begin(), identifier.end(), asciiDigit) &&
             !numericIdentifier(identifier))) {
            return false;
        }
        if (dot == std::string_view::npos) return true;
        begin = dot + 1U;
    }
}

}  // namespace

bool isPortablePackageId(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumComponentBytes)
        return false;
    std::size_t begin = 0U;
    for (;;) {
        const auto dot = value.find('.', begin);
        const auto segment =
            value.substr(begin, dot == std::string_view::npos
                                    ? std::string_view::npos
                                    : dot - begin);
        if (segment.empty() || !asciiAlphaNumeric(segment.front()) ||
            reservedWindowsBasename(segment) ||
            !std::all_of(segment.begin() + 1, segment.end(),
                         [](char character) {
                             return asciiAlphaNumeric(character) ||
                                    character == '_' || character == '-';
                         })) {
            return false;
        }
        if (dot == std::string_view::npos) return true;
        begin = dot + 1U;
    }
}

bool isCanonicalPackageVersion(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumComponentBytes)
        return false;
    const auto plus = value.find('+');
    if (plus != std::string_view::npos &&
        value.find('+', plus + 1U) != std::string_view::npos) {
        return false;
    }
    const auto hyphen = value.find('-');
    if (hyphen != std::string_view::npos &&
        plus != std::string_view::npos && hyphen > plus) {
        return false;
    }
    const auto coreEnd =
        std::min(hyphen == std::string_view::npos ? value.size() : hyphen,
                 plus == std::string_view::npos ? value.size() : plus);
    const auto core = value.substr(0U, coreEnd);
    const auto firstDot = core.find('.');
    const auto secondDot =
        firstDot == std::string_view::npos
            ? std::string_view::npos
            : core.find('.', firstDot + 1U);
    if (firstDot == std::string_view::npos ||
        secondDot == std::string_view::npos ||
        core.find('.', secondDot + 1U) != std::string_view::npos ||
        !numericIdentifier(core.substr(0U, firstDot)) ||
        !numericIdentifier(core.substr(firstDot + 1U,
                                       secondDot - firstDot - 1U)) ||
        !numericIdentifier(core.substr(secondDot + 1U))) {
        return false;
    }
    if (hyphen != std::string_view::npos) {
        const auto prereleaseEnd =
            plus == std::string_view::npos ? value.size() : plus;
        if (!validSemVerIdentifiers(
                value.substr(hyphen + 1U,
                             prereleaseEnd - hyphen - 1U),
                true)) {
            return false;
        }
    }
    return plus == std::string_view::npos ||
           validSemVerIdentifiers(value.substr(plus + 1U), false);
}

}  // namespace creator::avatar_pack_adapter::detail
