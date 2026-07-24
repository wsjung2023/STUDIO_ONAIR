#include "avatar_pack_adapter/CatalogRootAuthority.h"

#include <cerrno>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using creator::avatar_pack_adapter::detail::CatalogRootAuthority;
using creator::avatar_pack_adapter::detail::ensurePrivateDirectoryChild;

#ifndef _WIN32
int failuresRemaining = 0;

extern "C" int __real_fsync(int descriptor);

extern "C" int __wrap_fsync(int descriptor) {
    if (failuresRemaining > 0) {
        --failuresRemaining;
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}
#endif

}  // namespace

int main() {
#ifdef _WIN32
    return 77;
#else
    const auto parent =
        fs::temp_directory_path() /
        ("creator-catalog-durability-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove_all(parent, error);
    if (!fs::create_directory(parent)) return 2;
    fs::permissions(parent, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    if (error) return 3;

    const auto root = parent / "catalog";
    failuresRemaining = 1;
    if (CatalogRootAuthority::open(root).hasValue() ||
        !fs::is_directory(root)) {
        return 4;
    }
    failuresRemaining = 1;
    if (CatalogRootAuthority::open(root).hasValue()) return 5;
    failuresRemaining = 0;
    auto opened = CatalogRootAuthority::open(root);
    if (!opened.hasValue()) return 6;
    auto authority = std::move(opened).value();

    failuresRemaining = 1;
    if (authority.ensurePrivateChild("installed").hasValue() ||
        !fs::is_directory(root / "installed")) {
        return 7;
    }
    failuresRemaining = 1;
    if (authority.ensurePrivateChild("installed").hasValue()) return 8;
    failuresRemaining = 0;
    if (!authority.ensurePrivateChild("installed").hasValue()) return 9;

    const auto installed = root / "installed";
    failuresRemaining = 1;
    if (ensurePrivateDirectoryChild(installed, "vendor.foundation")
            .hasValue() ||
        !fs::is_directory(installed / "vendor.foundation")) {
        return 10;
    }
    failuresRemaining = 1;
    if (ensurePrivateDirectoryChild(installed, "vendor.foundation")
            .hasValue()) {
        return 11;
    }
    failuresRemaining = 0;
    if (!ensurePrivateDirectoryChild(installed, "vendor.foundation")
             .hasValue()) {
        return 12;
    }

    fs::remove_all(parent, error);
    return error ? 13 : 0;
#endif
}
