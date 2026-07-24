#include "avatar_pack_adapter/CatalogRootAuthority.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using creator::avatar_pack_adapter::detail::CatalogRootAuthority;

#ifndef _WIN32
bool childLockHasExpectedResult(const fs::path& root,
                                bool expectSuccess) {
    const auto child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        auto authority = CatalogRootAuthority::open(root);
        if (!authority.hasValue()) ::_exit(20);
        const auto locked = authority.value().lock();
        ::_exit(locked.hasValue() == expectSuccess ? 0 : 21);
    }
    int status = 0;
    return ::waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

}  // namespace

int main() {
#ifdef _WIN32
    return 77;
#else
    const auto parent =
        fs::temp_directory_path() /
        ("creator-catalog-authority-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove_all(parent, error);
    if (!fs::create_directory(parent)) return 2;
    fs::permissions(parent, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    if (error) return 3;

    const auto hostile = parent / "hostile";
    const auto branch = hostile / "branch";
    const auto privateParent = branch / "private";
    if (!fs::create_directories(privateParent)) return 13;
    fs::permissions(hostile, fs::perms::all,
                    fs::perm_options::replace, error);
    fs::permissions(branch, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    fs::permissions(privateParent, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    if (error) return 14;
    {
        auto hostileOpened =
            CatalogRootAuthority::open(privateParent / "catalog");
        if (hostileOpened.hasValue()) {
            auto hostileAuthority = std::move(hostileOpened).value();
            const auto displaced = hostile / "displaced";
            fs::rename(branch, displaced, error);
            if (error || !fs::create_directories(privateParent / "catalog"))
                return 15;
            fs::permissions(branch, fs::perms::owner_all,
                            fs::perm_options::replace, error);
            fs::permissions(privateParent, fs::perms::owner_all,
                            fs::perm_options::replace, error);
            fs::permissions(privateParent / "catalog",
                            fs::perms::owner_all,
                            fs::perm_options::replace, error);
            if (error || hostileAuthority.revalidate().hasValue())
                return 16;
        } else if (fs::exists(privateParent / "catalog")) {
            return 18;
        }
    }
    fs::remove_all(hostile, error);
    if (error) return 17;

    const auto root = parent / "catalog";
    auto opened = CatalogRootAuthority::open(root);
    if (!opened.hasValue()) return 4;
    auto authority = std::move(opened).value();
    {
        auto locked = authority.lock();
        if (!locked.hasValue()) return 5;
        if (!childLockHasExpectedResult(root, false)) return 6;
    }
    if (!childLockHasExpectedResult(root, true)) return 7;

    if (!fs::remove(root / "catalog.lock")) return 8;
    {
        std::ofstream replacement{root / "catalog.lock",
                                  std::ios::binary};
        replacement << "replacement";
    }
    fs::permissions(root / "catalog.lock",
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, error);
    if (error || authority.lock().hasValue()) return 9;

    const auto displaced = parent / "catalog-displaced";
    fs::rename(root, displaced, error);
    if (error || !fs::create_directory(root)) return 10;
    fs::permissions(root, fs::perms::owner_all,
                    fs::perm_options::replace, error);
    if (error || authority.revalidate().hasValue()) return 11;

    fs::remove_all(parent, error);
    return error ? 12 : 0;
#endif
}
