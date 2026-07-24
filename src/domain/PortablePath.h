#pragma once

#include <string>
#include <string_view>

namespace creator::domain {

/// Returns true only for one canonical, cross-platform, lowercase path
/// component. The accepted spelling is deliberately narrower than native
/// filesystems so Windows case/trailing-dot aliases and Unicode normalization
/// aliases cannot name the same project object differently.
[[nodiscard]] bool isPortableLowercasePathComponent(
    std::string_view value) noexcept;

/// Produces the Windows-style alias key used to detect an already-present
/// non-canonical spelling such as `Hero`, `hero.`, or `hero `.
[[nodiscard]] std::string portablePathAliasKey(std::string_view value);

}  // namespace creator::domain
