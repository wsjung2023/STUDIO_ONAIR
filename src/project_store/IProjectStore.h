#pragma once

#include "core/Result.h"
#include "domain/ProjectManifest.h"

#include <filesystem>
#include <string>

namespace creator::project_store {

/// Reads and writes the project package.
///
/// A working project is a folder, not a zip (ARCHITECTURE.md 7.1). Packing only
/// happens on export.
///
/// Writes must go through a temp file, flush and atomic rename (CLAUDE.md 4).
/// A half-written manifest.json is an unopenable project, and this is exactly
/// the file a crash is most likely to catch mid-write.
class IProjectStore {
public:
    virtual ~IProjectStore() = default;

    IProjectStore(const IProjectStore&) = delete;
    IProjectStore& operator=(const IProjectStore&) = delete;
    IProjectStore(IProjectStore&&) = delete;
    IProjectStore& operator=(IProjectStore&&) = delete;

    /// Creates the package directory tree and writes a fresh manifest.
    /// Fails with AlreadyExists if a manifest is already there - never
    /// overwrites, because that would discard someone's recording.
    [[nodiscard]] virtual creator::core::Result<creator::domain::ProjectManifest> create(
        const std::filesystem::path& packageDirectory, const std::string& name) = 0;

    /// Fails with NotFound if there is no manifest, ParseFailure if it is not
    /// readable JSON, or UnsupportedVersion if it was written by a newer build.
    [[nodiscard]] virtual creator::core::Result<creator::domain::ProjectManifest> load(
        const std::filesystem::path& packageDirectory) = 0;

    [[nodiscard]] virtual creator::core::Result<void> save(
        const std::filesystem::path& packageDirectory,
        const creator::domain::ProjectManifest& manifest) = 0;

protected:
    IProjectStore() = default;
};

}  // namespace creator::project_store
