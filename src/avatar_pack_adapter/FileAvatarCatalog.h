#pragma once

#include "avatar/IAvatarCatalog.h"
#include "avatar_pack_adapter/AvatarPackValidator.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace creator::avatar_pack_adapter {

enum class CatalogInstallOutcome {
    Installed,
    AlreadyInstalled,
    InstalledDurabilityIndeterminate,
};

/// Owns the private, immutable on-disk catalog of validated avatar packs.
///
/// `open` establishes and reconciles the catalog under its exclusive lock.
/// `install` validates a signed package with the Task 5 validator and consumes
/// its move-only staging capability at the atomic no-replace visibility point.
/// An indeterminate post-rename durability confirmation is a value outcome,
/// never an ordinary retryable failure; callers may continue using a target
/// only after this catalog's reconciliation has verified it.
///
/// Installed roots are private to the current OS user. A malicious process
/// running as that same user is outside the authority model.
///
/// The immediate parent of `catalogRoot` is a caller-owned durability
/// precondition: it must already exist as a private current-user directory,
/// and the caller is responsible for having made that parent's own creation
/// durable. `open` creates at most the final `catalogRoot` child, flushes the
/// parent entry, and retains no-follow parent and root handles for the
/// lifetime of this object (with no-delete sharing on Windows and identity
/// revalidation on POSIX). Paths returned by `payloadRoot` are valid authority
/// only for that same lifetime.
///
/// On POSIX, `open` also proves the absolute ancestry namespace-stable under
/// the catalog threat model. Every namespace parent is opened without
/// following symlinks, must be root/current-user owned, and must either deny
/// group/other writes or be sticky with a current-user-owned next child (for
/// example `/tmp`). Unsafe ancestry is rejected, and the accepted chain is
/// retained and revalidated for every operation.
class FileAvatarCatalog final : public avatar::IAvatarCatalog {
public:
    [[nodiscard]] static core::Result<FileAvatarCatalog> open(
        std::filesystem::path catalogRoot,
        std::vector<TrustedAvatarKey> trustedKeys) noexcept;

    FileAvatarCatalog(FileAvatarCatalog&&) noexcept;
    FileAvatarCatalog& operator=(FileAvatarCatalog&&) noexcept;
    ~FileAvatarCatalog() override;

    FileAvatarCatalog(const FileAvatarCatalog&) = delete;
    FileAvatarCatalog& operator=(const FileAvatarCatalog&) = delete;

    [[nodiscard]] core::Result<CatalogInstallOutcome> install(
        const std::filesystem::path& packagePath) noexcept;

    [[nodiscard]] core::Result<std::vector<avatar::AvatarAssetManifest>>
    list() const override;
    [[nodiscard]] core::Result<avatar::AvatarAssetManifest> find(
        const avatar::AvatarAssetId& id,
        std::string_view version) const override;
    [[nodiscard]] core::Result<std::filesystem::path> payloadRoot(
        const avatar::AvatarAssetId& id,
        std::string_view version) const override;

private:
    class Impl;
    explicit FileAvatarCatalog(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

}  // namespace creator::avatar_pack_adapter
