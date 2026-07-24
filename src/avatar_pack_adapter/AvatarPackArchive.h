#pragma once

#include "core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace creator::avatar_pack_adapter {

struct AvatarPackArchiveEntry final {
    std::uint32_t index{};
    std::string path;
    std::uint64_t compressedBytes{};
    std::uint64_t uncompressedBytes{};
};

/// Owns one miniz reader and exposes only preflight-validated regular entries.
class AvatarPackArchive final {
public:
    static constexpr std::uint32_t kMaximumEntryCount = 10'000U;
    static constexpr std::uint64_t kMaximumEntryBytes =
        512ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaximumAggregateBytes =
        2ULL * 1024ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kMaximumArchiveBytes =
        kMaximumAggregateBytes + 80ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaximumCentralDirectoryBytes =
        16U * 1024U * 1024U;
    static constexpr std::size_t kMaximumZipExtraBytes = 4096U;
    static constexpr std::size_t kMaximumZipCommentBytes = 4096U;
    static constexpr std::size_t kMaximumPathBytes = 1024U;

    [[nodiscard]] static core::Result<AvatarPackArchive> open(
        const std::filesystem::path& packagePath) noexcept;

    AvatarPackArchive(AvatarPackArchive&&) noexcept;
    AvatarPackArchive& operator=(AvatarPackArchive&&) noexcept;
    ~AvatarPackArchive();

    AvatarPackArchive(const AvatarPackArchive&) = delete;
    AvatarPackArchive& operator=(const AvatarPackArchive&) = delete;

    [[nodiscard]] const std::vector<AvatarPackArchiveEntry>& entries()
        const noexcept;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read(
        const AvatarPackArchiveEntry& entry, std::size_t maximumBytes) noexcept;
    using ChunkWriter = std::function<core::Result<void>(
        std::span<const std::uint8_t>)>;
    [[nodiscard]] core::Result<std::string> stream(
        const AvatarPackArchiveEntry& entry,
        std::uint64_t maximumExpandedBytes, const ChunkWriter& writer) noexcept;

private:
    class Impl;

    AvatarPackArchive(std::unique_ptr<Impl> implementation,
                      std::vector<AvatarPackArchiveEntry> entries);

    std::unique_ptr<Impl> implementation_;
    std::vector<AvatarPackArchiveEntry> entries_;
};

}  // namespace creator::avatar_pack_adapter
