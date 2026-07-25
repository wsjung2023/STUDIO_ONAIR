#include "project_store/AvatarSpecFileStore.h"

#include "avatar/AvatarSpec.h"
#include "core/AppError.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using creator::avatar::AssetRef;
using creator::avatar::AvatarAssetId;
using creator::avatar::AvatarId;
using creator::avatar::AvatarRepresentation;
using creator::avatar::AvatarSlot;
using creator::avatar::AvatarSpec;
using creator::avatar::AvatarSpecDraft;
using creator::avatar::RigFamily;
using creator::core::ErrorCode;
using creator::project_store::AvatarSpecFileStore;

#ifndef _WIN32
int fsyncFailuresRemaining = 0;
bool failNextRegularFileFsync = false;
bool growOnRead = false;
bool rebindOnAvatarOpen = false;
bool rebindAfterPromotion = false;
bool rebindEmptyListOnEof = false;
bool rebindOnMissingBackup = false;
bool introduceAliasAfterChildOpen = false;
bool failTemporaryCleanup = false;
enum class ReadMutation {
    None,
    AddHardLink,
    ReplaceWithSymlink,
};
ReadMutation readMutation = ReadMutation::None;
fs::path growthTarget;
fs::path readMutationTarget;
fs::path replacementFile;
fs::path displacedFile;
fs::path avatarDirectory;
fs::path displacedDirectory;
fs::path replacementDirectory;
fs::path rootDirectory;
fs::path displacedRoot;

extern "C" int __real_fsync(int descriptor);
extern "C" ssize_t __real_read(int descriptor, void* buffer,
                               std::size_t count);
extern "C" int __real_openat(int directory, const char* path, int flags, ...);
extern "C" int __real_renameat(int oldDirectory, const char* oldPath,
                                int newDirectory, const char* newPath);
extern "C" dirent* __real_readdir(DIR* directory);
extern "C" int __real_unlinkat(int directory, const char* path, int flags);

extern "C" int __wrap_fsync(int descriptor) {
    if (failNextRegularFileFsync) {
        struct stat information {};
        if (::fstat(descriptor, &information) == 0 &&
            S_ISREG(information.st_mode)) {
            failNextRegularFileFsync = false;
            errno = EIO;
            return -1;
        }
    }
    if (fsyncFailuresRemaining > 0) {
        --fsyncFailuresRemaining;
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}

extern "C" ssize_t __wrap_read(int descriptor, void* buffer,
                               std::size_t count) {
    const ssize_t result = __real_read(descriptor, buffer, count);
    if (result > 0 && growOnRead) {
        growOnRead = false;
        const int target =
            ::open(growthTarget.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (target >= 0) {
            constexpr char growth = ' ';
            (void)::write(target, &growth, 1U);
            (void)::close(target);
        }
    }
    if (result > 0 && readMutation != ReadMutation::None) {
        const ReadMutation requested = readMutation;
        readMutation = ReadMutation::None;
        std::error_code ignored;
        if (requested == ReadMutation::AddHardLink) {
            fs::create_hard_link(growthTarget, readMutationTarget, ignored);
        } else {
            fs::rename(growthTarget, displacedFile, ignored);
            if (!ignored) {
                fs::create_symlink(replacementFile, growthTarget, ignored);
            }
        }
    }
    return result;
}

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    const int result =
        (flags & O_CREAT) != 0
            ? __real_openat(directory, path, flags, mode)
            : __real_openat(directory, path, flags);
    if (result >= 0 && introduceAliasAfterChildOpen && path != nullptr &&
        std::strcmp(path, "hero") == 0 && (flags & O_DIRECTORY) != 0) {
        introduceAliasAfterChildOpen = false;
        std::error_code ignored;
        fs::create_directory(rootDirectory / "Hero", ignored);
    }
    if (result >= 0 && rebindOnAvatarOpen && path != nullptr &&
        std::strcmp(path, "avatar.json") == 0) {
        rebindOnAvatarOpen = false;
        std::error_code ignored;
        fs::rename(avatarDirectory, displacedDirectory, ignored);
        if (!ignored) {
            fs::create_directory_symlink(replacementDirectory,
                                         avatarDirectory, ignored);
        }
    }
    if (result < 0 && rebindOnMissingBackup && path != nullptr &&
        std::strcmp(path, "avatar.last-good.json") == 0) {
        rebindOnMissingBackup = false;
        std::error_code ignored;
        fs::rename(rootDirectory, displacedRoot, ignored);
        if (!ignored) fs::create_directory(rootDirectory, ignored);
    }
    return result;
}

extern "C" int __wrap_renameat(int oldDirectory, const char* oldPath,
                                int newDirectory, const char* newPath) {
    const int result =
        __real_renameat(oldDirectory, oldPath, newDirectory, newPath);
    if (result == 0 && rebindAfterPromotion && newPath != nullptr &&
        std::strcmp(newPath, "avatar.json") == 0) {
        rebindAfterPromotion = false;
        std::error_code ignored;
        fs::rename(avatarDirectory, displacedDirectory, ignored);
        if (!ignored) fs::create_directory(avatarDirectory, ignored);
    }
    return result;
}

extern "C" dirent* __wrap_readdir(DIR* directory) {
    dirent* result = __real_readdir(directory);
    if (result == nullptr && rebindEmptyListOnEof) {
        rebindEmptyListOnEof = false;
        std::error_code ignored;
        fs::rename(rootDirectory, displacedRoot, ignored);
        if (!ignored) fs::create_directory(rootDirectory, ignored);
    }
    return result;
}

extern "C" int __wrap_unlinkat(int directory, const char* path, int flags) {
    if (failTemporaryCleanup && path != nullptr &&
        std::strncmp(path, ".avatar.part-", 13U) == 0) {
        failTemporaryCleanup = false;
        errno = EACCES;
        return -1;
    }
    return __real_unlinkat(directory, path, flags);
}
#endif

AvatarSpec validSpec() {
    AvatarSpecDraft draft{
        .avatarId = AvatarId::create("hero").value(),
        .displayName = "Hero",
        .rigFamily = RigFamily::Humanoid,
        .speciesFamily = "human",
        .styleTheme = "studio",
        .preferredRepresentation = AvatarRepresentation::Inochi2d,
        .bodyMorphs = {},
        .faceMorphs = {},
        .animalMorphs = {},
        .slots = {
            {AvatarSlot::Body,
             AssetRef{AvatarAssetId::create("body").value(), "1.0.0",
                      "default"}},
            {AvatarSlot::Head,
             AssetRef{AvatarAssetId::create("head").value(), "1.0.0",
                      "default"}},
            {AvatarSlot::Eyes,
             AssetRef{AvatarAssetId::create("eyes").value(), "1.0.0",
                      "default"}},
            {AvatarSlot::Mouth,
             AssetRef{AvatarAssetId::create("mouth").value(), "1.0.0",
                      "default"}},
        },
        .palette = {},
        .materials = {},
        .expressions = {},
        .physics = {},
        .trackingProfileId = "default",
    };
    return AvatarSpec::create(std::move(draft)).value();
}

}  // namespace

int main() {
#ifdef _WIN32
    return 77;
#else
    const fs::path parent =
        fs::temp_directory_path() /
        ("creator-avatar-authority-" + std::to_string(::getpid()));
    std::error_code error;
    fs::remove_all(parent, error);
    if (!fs::create_directories(parent / "avatars")) return 2;
    const auto spec = validSpec();
    AvatarSpecFileStore store{parent / "avatars"};

    fsyncFailuresRemaining = 1;
    if (store.save(spec).hasValue() ||
        fs::exists(parent / "avatars" / "hero" / "avatar.json")) {
        return 3;
    }
    fsyncFailuresRemaining = 0;
    const auto initialSave = store.save(spec);
    if (!initialSave.hasValue()) {
        std::cerr << "INITIAL_SAVE_ERROR " << initialSave.error().message()
                  << '\n';
        return 4;
    }

    growthTarget = parent / "avatars" / "hero" / "avatar.json";
    fs::remove(parent / "avatars" / "hero" / "avatar.last-good.json",
               error);
    error.clear();
    growOnRead = true;
    const auto grown = store.load(spec.avatarId());
    if (grown.hasValue() || grown.error().code() != ErrorCode::IoFailure ||
        growOnRead) {
        return 5;
    }

    if (!store.save(spec).hasValue()) return 6;
    replacementDirectory = parent / "outside";
    if (!fs::create_directory(replacementDirectory)) return 7;
    fs::copy_file(parent / "avatars" / "hero" / "avatar.json",
                  replacementDirectory / "avatar.json");
    avatarDirectory = parent / "avatars" / "hero";
    displacedDirectory = parent / "avatars" / "hero-displaced";
    rebindOnAvatarOpen = true;
    const auto rebound = store.load(spec.avatarId());
    if (rebound.hasValue() ||
        rebound.error().code() != ErrorCode::InvalidArgument ||
        rebindOnAvatarOpen ||
        !fs::is_symlink(avatarDirectory) ||
        !fs::exists(displacedDirectory / "avatar.json")) {
        return 8;
    }

    int reviewerFailures = 0;
    const auto expectReviewerCase = [&](bool passed, const char* name) {
        if (passed) return;
        ++reviewerFailures;
        std::cerr << "REVIEWER_RED " << name << '\n';
    };

    const fs::path promotionRoot = parent / "promotion" / "avatars";
    if (!fs::create_directories(promotionRoot)) return 10;
    AvatarSpecFileStore promotionStore{promotionRoot};
    if (!promotionStore.save(spec).hasValue()) return 11;
    avatarDirectory = promotionRoot / "hero";
    displacedDirectory = promotionRoot / "hero-promoted";
    rebindAfterPromotion = true;
    const auto promoted = promotionStore.save(spec);
    expectReviewerCase(
        !promoted.hasValue() &&
            promoted.error().code() == ErrorCode::InvalidArgument &&
            !rebindAfterPromotion,
        "post-promotion-rebind");

    const fs::path hardLinkRoot = parent / "read-hardlink" / "avatars";
    if (!fs::create_directories(hardLinkRoot)) return 13;
    AvatarSpecFileStore hardLinkStore{hardLinkRoot};
    if (!hardLinkStore.save(spec).hasValue()) return 14;
    growthTarget = hardLinkRoot / "hero" / "avatar.json";
    readMutationTarget = parent / "read-hardlink" / "outside-link.json";
    readMutation = ReadMutation::AddHardLink;
    const auto hardLinked = hardLinkStore.load(spec.avatarId());
    expectReviewerCase(
        !hardLinked.hasValue() &&
            hardLinked.error().code() == ErrorCode::InvalidArgument &&
            readMutation == ReadMutation::None,
        "hardlink-during-read");

    const fs::path replacementRoot = parent / "read-replace" / "avatars";
    if (!fs::create_directories(replacementRoot)) return 16;
    AvatarSpecFileStore replacementStore{replacementRoot};
    if (!replacementStore.save(spec).hasValue()) return 17;
    growthTarget = replacementRoot / "hero" / "avatar.json";
    replacementFile = parent / "read-replace" / "outside.json";
    displacedFile = parent / "read-replace" / "original.json";
    fs::copy_file(growthTarget, replacementFile);
    readMutation = ReadMutation::ReplaceWithSymlink;
    const auto replaced = replacementStore.load(spec.avatarId());
    expectReviewerCase(
        !replaced.hasValue() &&
            replaced.error().code() == ErrorCode::InvalidArgument &&
            readMutation == ReadMutation::None,
        "symlink-replacement-during-read");

    const fs::path emptyListRoot = parent / "list-empty" / "avatars";
    if (!fs::create_directories(emptyListRoot)) return 19;
    AvatarSpecFileStore emptyListStore{emptyListRoot};
    rootDirectory = emptyListRoot;
    displacedRoot = parent / "list-empty" / "avatars-displaced";
    rebindEmptyListOnEof = true;
    const auto emptyListed = emptyListStore.list();
    expectReviewerCase(
        !emptyListed.hasValue() &&
            emptyListed.error().code() == ErrorCode::InvalidArgument &&
            !rebindEmptyListOnEof,
        "empty-list-root-rebind");

    const fs::path lastListRoot = parent / "list-last" / "avatars";
    if (!fs::create_directories(lastListRoot / "hero")) return 21;
    AvatarSpecFileStore lastListStore{lastListRoot};
    rootDirectory = lastListRoot;
    displacedRoot = parent / "list-last" / "avatars-displaced";
    rebindOnMissingBackup = true;
    const auto lastListed = lastListStore.list();
    expectReviewerCase(
        !lastListed.hasValue() &&
            lastListed.error().code() == ErrorCode::InvalidArgument &&
            !rebindOnMissingBackup,
        "last-entry-list-root-rebind");

    const fs::path aliasRoot = parent / "alias-after-open" / "avatars";
    if (!fs::create_directories(aliasRoot)) return 23;
    AvatarSpecFileStore aliasStore{aliasRoot};
    rootDirectory = aliasRoot;
    introduceAliasAfterChildOpen = true;
    const auto aliased = aliasStore.save(spec);
    expectReviewerCase(
        !aliased.hasValue() &&
            aliased.error().code() == ErrorCode::InvalidArgument &&
            !introduceAliasAfterChildOpen &&
            fs::is_directory(aliasRoot / "Hero"),
        "alias-after-child-open");

    const fs::path cleanupRoot = parent / "cleanup" / "avatars";
    if (!fs::create_directories(cleanupRoot)) return 25;
    AvatarSpecFileStore cleanupStore{cleanupRoot};
    if (!cleanupStore.save(spec).hasValue()) return 26;
    failNextRegularFileFsync = true;
    failTemporaryCleanup = true;
    const auto cleanupFailed = cleanupStore.save(spec);
    expectReviewerCase(
        !cleanupFailed.hasValue() &&
            cleanupFailed.error().message().find("cleanup") !=
                std::string::npos &&
            !failNextRegularFileFsync && !failTemporaryCleanup,
        "cleanup-failure-surfaced");
    bool foundResidue = false;
    for (const auto& entry :
         fs::directory_iterator(cleanupRoot / "hero")) {
        if (entry.path().filename().string().starts_with(".avatar.part-")) {
            foundResidue = true;
        }
    }
    expectReviewerCase(foundResidue, "cleanup-residue-observable");

    fs::remove_all(parent, error);
    if (error) return 9;
    return reviewerFailures == 0 ? 0 : 30 + reviewerFailures;
#endif
}
