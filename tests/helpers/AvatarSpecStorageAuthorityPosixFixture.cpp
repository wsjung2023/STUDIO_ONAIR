#include "project_store/AvatarSpecFileStore.h"

#include "avatar/AvatarSpec.h"
#include "core/AppError.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
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
bool growOnRead = false;
bool rebindOnAvatarOpen = false;
fs::path growthTarget;
fs::path avatarDirectory;
fs::path displacedDirectory;
fs::path replacementDirectory;

extern "C" int __real_fsync(int descriptor);
extern "C" ssize_t __real_read(int descriptor, void* buffer,
                               std::size_t count);
extern "C" int __real_openat(int directory, const char* path, int flags, ...);

extern "C" int __wrap_fsync(int descriptor) {
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
    if (rebindOnAvatarOpen && path != nullptr &&
        std::strcmp(path, "avatar.json") == 0) {
        rebindOnAvatarOpen = false;
        std::error_code ignored;
        fs::rename(avatarDirectory, displacedDirectory, ignored);
        if (!ignored) {
            fs::create_directory_symlink(replacementDirectory,
                                         avatarDirectory, ignored);
        }
    }
    return (flags & O_CREAT) != 0
               ? __real_openat(directory, path, flags, mode)
               : __real_openat(directory, path, flags);
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
    if (!store.save(spec).hasValue()) return 4;

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

    fs::remove_all(parent, error);
    return error ? 9 : 0;
#endif
}
