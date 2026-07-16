#pragma once

#include "core/Result.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace creator::project_store::internal {

class SqliteConnection;

struct MigrationDescriptor final {
    std::int32_t version;
    std::string_view name;
    std::string_view checksum;
    std::string_view sql;
};

[[nodiscard]] core::Result<void> applyMigrations(
    SqliteConnection& connection, std::span<const MigrationDescriptor> migrations);
[[nodiscard]] std::span<const MigrationDescriptor> defaultMigrations() noexcept;

}  // namespace creator::project_store::internal

namespace creator::project_store {

class MigrationRunner final {
public:
    static constexpr std::int32_t kLatestVersion = 2;

    [[nodiscard]] static core::Result<void> apply(internal::SqliteConnection& connection);
};

}  // namespace creator::project_store
