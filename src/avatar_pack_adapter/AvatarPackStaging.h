#pragma once

#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace creator::avatar_pack_adapter {

class AvatarPackArchive;
class AvatarPackValidator;
struct AvatarPackArchiveEntry;

/// Move-only ownership capability for one verified private staging tree.
///
/// Move construction/assignment transfers ownership. Task 6 may accept this
/// capability by move to extend secure promotion without converting it to a
/// path.
class AvatarPackStaging final {
public:
    AvatarPackStaging(AvatarPackStaging&&) noexcept;
    AvatarPackStaging& operator=(AvatarPackStaging&&) noexcept;
    ~AvatarPackStaging();

    AvatarPackStaging(const AvatarPackStaging&) = delete;
    AvatarPackStaging& operator=(const AvatarPackStaging&) = delete;

    /// Returns a diagnostic/display path, never file-access authority.
    [[nodiscard]] const std::filesystem::path& displayPath() const noexcept;
    [[nodiscard]] core::Result<bool>
    exists(std::string_view relativePath) const noexcept;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>>
    read(std::string_view relativePath,
         std::size_t maximumBytes) const noexcept;

    /// Deletes the owned tree only while retained root identity still matches.
    [[nodiscard]] core::Result<void> cleanup() noexcept;

private:
    friend class AvatarPackValidator;

    [[nodiscard]] static core::Result<AvatarPackStaging>
    create(const std::filesystem::path& parent);

    [[nodiscard]] core::Result<void> writeNewFile(
        std::string_view relativePath,
        std::span<const std::uint8_t> bytes);
    [[nodiscard]] core::Result<std::string> extractNewFile(
        AvatarPackArchive& archive, const AvatarPackArchiveEntry& entry,
        std::uint64_t maximumExpandedBytes);
    [[nodiscard]] core::Result<void> seal();
    class Impl;

    explicit AvatarPackStaging(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

}  // namespace creator::avatar_pack_adapter
