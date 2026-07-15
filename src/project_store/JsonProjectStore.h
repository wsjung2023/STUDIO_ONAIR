#pragma once

#include "project_store/IProjectStore.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace creator::project_store {

/// IProjectStore backed by manifest.json on the filesystem.
///
/// Uses std::filesystem and nlohmann/json rather than QDir and QJsonDocument.
/// Qt here would make cs_project_store a Qt-linking target and collapse the
/// module split (ARCHITECTURE.md 14, CLAUDE.md 5).
///
/// Only manifest.json is handled. project.db and the SQLite migrations are
/// R0-02.
class JsonProjectStore final : public IProjectStore {
public:
    static constexpr std::string_view kManifestFileName = "manifest.json";

    [[nodiscard]] creator::core::Result<creator::domain::ProjectManifest> create(
        const std::filesystem::path& packageDirectory, const std::string& name) override;

    [[nodiscard]] creator::core::Result<creator::domain::ProjectManifest> load(
        const std::filesystem::path& packageDirectory) override;

    [[nodiscard]] creator::core::Result<void> save(
        const std::filesystem::path& packageDirectory,
        const creator::domain::ProjectManifest& manifest) override;
};

}  // namespace creator::project_store
