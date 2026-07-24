#include "domain/PortablePath.h"

#include <algorithm>
#include <array>

namespace creator::domain {
namespace {

constexpr std::size_t kMaximumComponentBytes = 128U;

bool reservedWindowsBasename(std::string_view value) noexcept {
    constexpr std::array<std::string_view, 22U> reserved{
        "con",  "prn",  "aux",  "nul",  "com1", "com2",
        "com3", "com4", "com5", "com6", "com7", "com8",
        "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
        "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::find(reserved.begin(), reserved.end(), value) !=
           reserved.end();
}

}  // namespace

std::string portablePathAliasKey(std::string_view value) {
    while (!value.empty() &&
           (value.back() == '.' || value.back() == ' ')) {
        value.remove_suffix(1U);
    }
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        if (byte >= 'A' && byte <= 'Z') {
            result.push_back(static_cast<char>(byte - 'A' + 'a'));
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

bool isPortableLowercasePathComponent(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumComponentBytes ||
        value == "." || value == "..") {
        return false;
    }
    if (reservedWindowsBasename(value)) return false;
    if (!((value.front() >= 'a' && value.front() <= 'z') ||
          (value.front() >= '0' && value.front() <= '9'))) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_';
    });
}

}  // namespace creator::domain
