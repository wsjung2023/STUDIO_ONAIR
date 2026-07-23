#pragma once

#include "avatar/AvatarSpec.h"

#include <filesystem>

#include <nlohmann/json.hpp>

namespace creator::avatar {

/// Converts validated engine-neutral avatar specifications to and from canonical JSON files.
class AvatarSpecCodec final {
public:
    [[nodiscard]] nlohmann::json toJson(const AvatarSpec& spec) const;
    [[nodiscard]] core::Result<AvatarSpec> fromJson(const nlohmann::json& json) const;
    [[nodiscard]] core::Result<AvatarSpec> load(const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> save(const std::filesystem::path& path,
                                          const AvatarSpec& spec) const;
};

}  // namespace creator::avatar
