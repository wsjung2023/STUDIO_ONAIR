#pragma once

#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace creator::avatar_pack_adapter {

class AvatarPackArchive;
struct AvatarPackArchiveEntry;

/// Creates and owns one handle-verified, private extraction directory.
class AvatarPackStaging final {
public:
    [[nodiscard]] static core::Result<AvatarPackStaging> create(
        const std::filesystem::path& parent);

    AvatarPackStaging(AvatarPackStaging&&) noexcept;
    AvatarPackStaging& operator=(AvatarPackStaging&&) noexcept;
    ~AvatarPackStaging();

    AvatarPackStaging(const AvatarPackStaging&) = delete;
    AvatarPackStaging& operator=(const AvatarPackStaging&) = delete;

    [[nodiscard]] core::Result<void> writeNewFile(
        std::string_view relativePath,
        std::span<const std::uint8_t> bytes);
    [[nodiscard]] core::Result<std::string> extractNewFile(
        AvatarPackArchive& archive, const AvatarPackArchiveEntry& entry,
        std::uint64_t maximumExpandedBytes);

    /// Deletes every object created by this staging instance.
    ///
    /// A cleanup failure is returned explicitly and is never hidden by the
    /// validation error that initiated cleanup.
    [[nodiscard]] core::Result<void> cleanup();

    /// Closes the defensive handles and transfers directory ownership.
    [[nodiscard]] core::Result<std::filesystem::path> finish();

private:
    class Impl;

    explicit AvatarPackStaging(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

}  // namespace creator::avatar_pack_adapter
