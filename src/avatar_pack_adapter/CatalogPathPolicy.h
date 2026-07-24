#pragma once

#include <string_view>

namespace creator::avatar_pack_adapter::detail {

/// Portable catalog directory spelling for a signed package identity.
[[nodiscard]] bool isPortablePackageId(std::string_view value) noexcept;

/// Canonical lowercase-ASCII SemVer spelling for a catalog version directory.
[[nodiscard]] bool isCanonicalPackageVersion(
    std::string_view value) noexcept;

}  // namespace creator::avatar_pack_adapter::detail
