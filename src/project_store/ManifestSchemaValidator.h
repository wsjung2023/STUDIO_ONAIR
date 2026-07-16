#pragma once

#include "core/Result.h"

#include <nlohmann/json_fwd.hpp>

namespace creator::project_store {

[[nodiscard]] creator::core::Result<void> validateManifestJson(
    const nlohmann::json& document);

}  // namespace creator::project_store
