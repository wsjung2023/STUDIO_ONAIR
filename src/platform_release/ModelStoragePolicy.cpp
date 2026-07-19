#include "platform_release/ModelStoragePolicy.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::platform_release {
namespace {

bool canonicalSha256(const std::string& value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

bool pathWithin(const std::filesystem::path& root,
                const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

}  // namespace

core::Result<ModelInstallPlan> ModelStoragePolicy::admit(
    const ModelInstallRequest& request,
    const ModelStorageSnapshot& storage) const {
    if (modelRoot_.empty() || request.finalPath.empty() ||
        maximumModelBytes_ == 0 || request.downloadBytes == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model storage request is incomplete"};
    }
    if (!canonicalSha256(request.expectedSha256)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (request.downloadBytes > maximumModelBytes_ ||
        storage.installedModelBytes > maximumModelBytes_ ||
        request.downloadBytes >
            maximumModelBytes_ - storage.installedModelBytes) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "model exceeds the device model-storage budget"};
    }
    if (storage.availableBytes < request.downloadBytes) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "device has insufficient free space for the model"};
    }

    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(modelRoot_, error);
    if (error || !std::filesystem::is_directory(canonicalRoot, error)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model storage root is unavailable"};
    }
    const auto canonicalFinal =
        std::filesystem::weakly_canonical(request.finalPath, error);
    if (error || !pathWithin(canonicalRoot, canonicalFinal)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model path resolves outside the configured root"};
    }
    if (!std::filesystem::is_directory(canonicalFinal.parent_path(), error) ||
        error) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model destination directory is unavailable"};
    }

    auto staging = canonicalFinal;
    staging += ".part";
    return ModelInstallPlan{
        .stagingPath = std::move(staging),
        .finalPath = canonicalFinal,
        .expectedSha256 = request.expectedSha256,
        .expectedBytes = request.downloadBytes,
    };
}

core::Result<void> ModelStoragePolicy::publishVerified(
    const ModelInstallPlan& plan) const {
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(modelRoot_, error);
    if (error || !std::filesystem::is_directory(canonicalRoot, error)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model storage root is unavailable"};
    }
    error.clear();
    const auto canonicalFinal = std::filesystem::weakly_canonical(
        plan.finalPath, error);
    if (error) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model publication destination is unavailable"};
    }
    error.clear();
    const auto canonicalStaging = std::filesystem::weakly_canonical(
        plan.stagingPath, error);
    auto expectedStaging = canonicalFinal;
    expectedStaging += ".part";
    if (error || !pathWithin(canonicalRoot, canonicalFinal) ||
        !pathWithin(canonicalRoot, canonicalStaging) ||
        canonicalStaging != expectedStaging || plan.expectedBytes == 0 ||
        !canonicalSha256(plan.expectedSha256)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "model publication plan is invalid"};
    }

    const auto bytes = std::filesystem::file_size(canonicalStaging, error);
    if (error || bytes != plan.expectedBytes) {
        std::filesystem::remove(canonicalStaging, error);
        return core::AppError{core::ErrorCode::IoFailure,
                              "staged model size does not match its manifest"};
    }
    const auto hash = core::sha256File(canonicalStaging);
    if (!hash.hasValue() || hash.value() != plan.expectedSha256) {
        std::filesystem::remove(canonicalStaging, error);
        return core::AppError{core::ErrorCode::IoFailure,
                              "staged model SHA-256 does not match its manifest"};
    }

#ifdef _WIN32
    if (!MoveFileExW(canonicalStaging.c_str(), canonicalFinal.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "verified model could not be atomically published"};
    }
#else
    std::filesystem::rename(canonicalStaging, canonicalFinal, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "verified model could not be atomically published"};
    }
#endif
    return core::ok();
}

}  // namespace creator::platform_release
