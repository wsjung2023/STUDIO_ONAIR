#pragma once

#include "core/Result.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace creator::avatar_pack_adapter::detail {

class CatalogOperationLock final {
public:
    CatalogOperationLock(CatalogOperationLock&&) noexcept;
    CatalogOperationLock& operator=(CatalogOperationLock&&) noexcept;
    ~CatalogOperationLock();

    CatalogOperationLock(const CatalogOperationLock&) = delete;
    CatalogOperationLock& operator=(const CatalogOperationLock&) = delete;

private:
    class Impl;
    explicit CatalogOperationLock(std::unique_ptr<Impl> implementation);
    friend class CatalogRootAuthority;

    std::unique_ptr<Impl> implementation_;
};

/// Retained authority for one catalog root and its already-existing parent.
class CatalogRootAuthority final {
public:
    [[nodiscard]] static core::Result<CatalogRootAuthority> open(
        std::filesystem::path rootPath) noexcept;

    CatalogRootAuthority(CatalogRootAuthority&&) noexcept;
    CatalogRootAuthority& operator=(CatalogRootAuthority&&) noexcept;
    ~CatalogRootAuthority();

    CatalogRootAuthority(const CatalogRootAuthority&) = delete;
    CatalogRootAuthority& operator=(const CatalogRootAuthority&) = delete;

    [[nodiscard]] const std::filesystem::path& rootPath() const noexcept;
    [[nodiscard]] core::Result<void> revalidate() const noexcept;
    [[nodiscard]] core::Result<CatalogOperationLock> lock() const noexcept;
    [[nodiscard]] core::Result<void> ensurePrivateChild(
        std::string_view name) const noexcept;

private:
    class Impl;
    explicit CatalogRootAuthority(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] bool isTrustedPrivateDirectory(
    const std::filesystem::path& path) noexcept;

/// Creates or validates exactly one child beneath a retained trusted parent.
[[nodiscard]] core::Result<void> ensurePrivateDirectoryChild(
    const std::filesystem::path& parent,
    std::string_view childName) noexcept;

}  // namespace creator::avatar_pack_adapter::detail
