#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string_view>

namespace creator::project_store::internal {

[[nodiscard]] core::Result<void> writeFileDurably(
    const std::filesystem::path& target, std::string_view contents);

}  // namespace creator::project_store::internal
