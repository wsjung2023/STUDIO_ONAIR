#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string_view>

namespace creator::project_store::internal {

/// Creates or validates one canonical direct child beneath a retained plain
/// project directory and makes the directory entry durable before returning.
[[nodiscard]] core::Result<void> ensureAvatarDirectoryDurably(
    const std::filesystem::path& projectDirectory,
    std::string_view childName) noexcept;

}  // namespace creator::project_store::internal
