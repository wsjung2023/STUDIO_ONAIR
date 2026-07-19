#include "platform_release/DiagnosticBundle.h"

#include "core/AppError.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace creator::platform_release {
namespace {

namespace fs = std::filesystem;
constexpr std::uintmax_t kMaximumFileBytes = 4U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumBundleBytes = 8U * 1024U * 1024U;
constexpr std::array<std::string_view, 3> kAllowedNames{
    "application.log", "release-manifest.json", "system-summary.json"};
constexpr std::array<std::string_view, 12> kForbiddenContentMarkers{
    "\"transcript\"", "\"recordingpath\"", "\"projectpath\"",
    "\"projectcontent\"", "\"cursorevents\"", "\"captiontext\"",
    "\"mediapath\"", "\"authorization\"", "\"access_token\"",
    "\"api_key\"", "\"password\"", "\"secret\""};

struct PreparedFile final {
    std::string name;
    std::string contents;
};

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(fs::path path) : path_(std::move(path)) {}
    ~TemporaryDirectory() {
        if (active_) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    void release() noexcept { active_ = false; }

private:
    fs::path path_;
    bool active_{true};
};

bool isAllowedName(std::string_view name) {
    return std::find(kAllowedNames.begin(), kAllowedNames.end(), name) !=
           kAllowedNames.end();
}

bool isCanonicalRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

bool isWithin(const fs::path& child, const fs::path& root) {
    auto childPart = child.begin();
    for (auto rootPart = root.begin(); rootPart != root.end();
         ++rootPart, ++childPart) {
        if (childPart == child.end() || *childPart != *rootPart) return false;
    }
    return true;
}

bool containsSymlink(const fs::path& root, const fs::path& relative) {
    auto candidate = root;
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(candidate, error)) || error) return true;
    for (const auto& part : relative) {
        candidate /= part;
        if (fs::is_symlink(fs::symlink_status(candidate, error)) || error) return true;
    }
    return false;
}

bool containsPrivateContent(const std::string& contents) {
    if (contents.find('\0') != std::string::npos) return true;
    std::string lowered{contents};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return std::any_of(kForbiddenContentMarkers.begin(),
                       kForbiddenContentMarkers.end(),
                       [&](std::string_view marker) {
                           return lowered.find(marker) != std::string::npos;
                       });
}

core::Result<void> makeFileDurable(const fs::path& path) {
#if defined(_WIN32)
    const auto handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic bundle file could not be reopened"};
    }
    const auto flushed = ::FlushFileBuffers(handle) != 0;
    ::CloseHandle(handle);
    if (!flushed) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic bundle file could not be made durable"};
    }
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic bundle file could not be reopened"};
    }
    const auto flushed = ::fsync(descriptor) == 0;
    ::close(descriptor);
    if (!flushed) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic bundle file could not be made durable"};
    }
#endif
    return core::ok();
}

core::Result<void> writeDurably(const fs::path& path, std::string_view contents) {
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "diagnostic bundle file could not be opened"};
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output.good()) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "diagnostic bundle file could not be written"};
        }
    }
    return makeFileDurable(path);
}

core::Result<std::vector<PreparedFile>> prepareFiles(
    const DiagnosticBundleRequest& request, const fs::path& canonicalRoot) {
    if (request.files.empty() || request.files.size() > kAllowedNames.size()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "diagnostic bundle file count is invalid"};
    }
    std::set<std::string> names;
    std::vector<PreparedFile> prepared;
    std::uintmax_t totalBytes = 0;
    for (const auto& relative : request.files) {
        const auto name = relative.filename().string();
        if (!isCanonicalRelativePath(relative) || !isAllowedName(name) ||
            !names.insert(name).second || containsSymlink(canonicalRoot, relative)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "diagnostic input is not an allowlisted regular file"};
        }
        std::error_code error;
        const auto source = fs::weakly_canonical(canonicalRoot / relative, error);
        if (error || !isWithin(source, canonicalRoot) || !fs::is_regular_file(source, error) ||
            error || fs::hard_link_count(source, error) != 1 || error) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "diagnostic input escapes its source root or is linked"};
        }
        const auto size = fs::file_size(source, error);
        if (error || size > kMaximumFileBytes || totalBytes > kMaximumBundleBytes - size) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "diagnostic input exceeds the local bundle budget"};
        }
        std::ifstream input{source, std::ios::binary};
        if (!input) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "diagnostic input could not be read"};
        }
        std::string contents(static_cast<std::size_t>(size), '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size())) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "diagnostic input changed while being read"};
        }
        if (containsPrivateContent(contents)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "diagnostic input contains private content fields"};
        }
        totalBytes += size;
        prepared.push_back({name, std::move(contents)});
    }
    std::sort(prepared.begin(), prepared.end(),
              [](const PreparedFile& left, const PreparedFile& right) {
                  return left.name < right.name;
              });
    return prepared;
}

}  // namespace

core::Result<fs::path> DiagnosticBundle::create(
    const DiagnosticBundleRequest& request) {
    if (!request.consentGranted) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "diagnostic bundle requires explicit user consent"};
    }
    if (request.sourceRoot.empty() || request.destination.empty() ||
        request.destination.filename().empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "diagnostic source and destination are required"};
    }
    std::error_code error;
    if (fs::exists(request.destination, error) || error) {
        return core::AppError{core::ErrorCode::AlreadyExists,
                              "diagnostic destination already exists"};
    }
    const auto canonicalRoot = fs::canonical(request.sourceRoot, error);
    if (error || !fs::is_directory(canonicalRoot, error) || error ||
        fs::is_symlink(fs::symlink_status(request.sourceRoot, error)) || error) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "diagnostic source root is invalid"};
    }
    const auto prepared = prepareFiles(request, canonicalRoot);
    if (!prepared.hasValue()) return prepared.error();

    const auto parent = request.destination.has_parent_path()
                            ? request.destination.parent_path()
                            : fs::current_path(error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic destination parent is unavailable"};
    }
    fs::create_directories(parent, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic destination parent could not be created"};
    }
    const auto temporary =
        parent / ("." + request.destination.filename().string() + ".part-" +
                  core::generateUuidV4());
    fs::create_directory(temporary, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic temporary directory could not be created"};
    }
    TemporaryDirectory cleanup{temporary};

    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : prepared.value()) {
        const auto output = temporary / file.name;
        const auto written = writeDurably(output, file.contents);
        if (!written.hasValue()) return written.error();
        const auto hash = core::sha256File(output);
        if (!hash.hasValue()) return hash.error();
        files.push_back({{"name", file.name}, {"sha256", hash.value()}});
    }
    const nlohmann::json manifest{{"files", std::move(files)},
                                  {"schemaVersion", 1}};
    const auto manifestPath = temporary / "diagnostic-bundle-manifest.json";
    const auto manifestWritten = writeDurably(manifestPath, manifest.dump(2) + "\n");
    if (!manifestWritten.hasValue()) return manifestWritten.error();

    fs::rename(temporary, request.destination, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "diagnostic bundle could not be atomically published"};
    }
    cleanup.release();
    return request.destination;
}

}  // namespace creator::platform_release
