#include "ffmpeg_adapter/FfmpegMediaProbe.h"

#include "core/AppError.h"
#include "core/Sha256.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::ffmpeg_adapter {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}

AppError ioFailure(std::string message) {
    return {ErrorCode::IoFailure, std::move(message)};
}

AppError parseFailure(std::string message) {
    return {ErrorCode::ParseFailure, std::move(message)};
}

AppError filesystemFailure(std::string action, const std::error_code& error) {
    if (error == std::errc::no_such_file_or_directory) {
        return {ErrorCode::NotFound, std::move(action)};
    }
    return ioFailure(std::move(action) + ": " + error.message());
}

bool isRedirect(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, error));
#endif
}

bool samePathComponent(const std::filesystem::path& first,
                       const std::filesystem::path& second) {
#ifdef _WIN32
    const auto& firstText = first.native();
    const auto& secondText = second.native();
    if (firstText.size() > static_cast<std::size_t>(
                               std::numeric_limits<int>::max()) ||
        secondText.size() > static_cast<std::size_t>(
                                std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               firstText.c_str(), static_cast<int>(firstText.size()),
               secondText.c_str(), static_cast<int>(secondText.size()), TRUE) ==
           CSTR_EQUAL;
#else
    return first == second;
#endif
}

bool isWithin(const std::filesystem::path& root,
              const std::filesystem::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            !samePathComponent(*rootPart, *candidatePart)) {
            return false;
        }
    }
    return true;
}

struct ValidatedMediaPath final {
    std::filesystem::path root;
    std::filesystem::path file;
};

Result<ValidatedMediaPath> validatedMediaPath(
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& relativePath) {
    if (relativePath.empty() || relativePath.is_absolute() ||
        relativePath.has_root_name() || relativePath.has_root_directory() ||
        relativePath != relativePath.lexically_normal()) {
        return invalid("media path must be a normalized package-relative path");
    }
    for (const auto& component : relativePath) {
        if (component == "." || component == ".." || component.empty()) {
            return invalid("media path escapes its package root");
        }
    }

    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(packageRoot, error);
    if (error) {
        return filesystemFailure("media package root could not be inspected",
                                 error);
    }
    if (rootStatus.type() == std::filesystem::file_type::not_found) {
        return AppError{ErrorCode::NotFound,
                        "media package root was not found"};
    }
    if (std::filesystem::is_symlink(rootStatus) || isRedirect(packageRoot)) {
        return invalid("media package root is redirected");
    }
    const auto root = std::filesystem::weakly_canonical(packageRoot, error);
    if (error) {
        return filesystemFailure("media package root could not be resolved",
                                 error);
    }
    const bool rootIsDirectory = std::filesystem::is_directory(root, error);
    if (error) {
        return filesystemFailure("media package root could not be inspected",
                                 error);
    }
    if (!rootIsDirectory) {
        return invalid("media package root is not a directory");
    }

    auto current = root;
    for (const auto& component : relativePath) {
        current /= component;
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) {
            return filesystemFailure("media path could not be inspected", error);
        }
        if (status.type() == std::filesystem::file_type::not_found) {
            return AppError{ErrorCode::NotFound, "media file was not found"};
        }
        if (std::filesystem::is_symlink(status) || isRedirect(current)) {
            return invalid("media path contains a redirected component");
        }
    }
    if (!std::filesystem::is_regular_file(current, error) || error) {
        if (error) {
            return filesystemFailure("media file type could not be inspected",
                                     error);
        }
        return invalid("media path does not name a regular file");
    }
    const auto links = std::filesystem::hard_link_count(current, error);
    if (error) {
        return filesystemFailure("media file links could not be inspected",
                                 error);
    }
    if (links != 1) {
        return invalid("media files must not have hard links");
    }
    const auto canonical = std::filesystem::weakly_canonical(current, error);
    if (error) {
        return filesystemFailure("media file could not be resolved", error);
    }
    if (!isWithin(root, canonical)) {
        return invalid("media path escapes its package root");
    }
    return ValidatedMediaPath{.root = root, .file = canonical};
}

#ifdef _WIN32
class LockedMediaPath final : public media::IMediaIdentityLease {
public:
    LockedMediaPath() = default;
    LockedMediaPath(const LockedMediaPath&) = delete;
    LockedMediaPath& operator=(const LockedMediaPath&) = delete;
    LockedMediaPath(LockedMediaPath&& other) noexcept
        : handles_(std::move(other.handles_)), byteSize_(other.byteSize_) {
        other.handles_.clear();
    }
    LockedMediaPath& operator=(LockedMediaPath&&) = delete;
    ~LockedMediaPath() {
        for (const HANDLE handle : handles_) CloseHandle(handle);
    }

    void add(HANDLE handle) { handles_.push_back(handle); }
    void setByteSize(std::uint64_t byteSize) noexcept { byteSize_ = byteSize; }
    [[nodiscard]] std::uint64_t byteSize() const noexcept { return byteSize_; }
    [[nodiscard]] Result<void> verifyCurrentIdentity() const override {
        if (handles_.empty()) {
            return ioFailure("media identity lease is not active");
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handles_.back(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            information.nNumberOfLinks != 1) {
            return ioFailure("media identity changed before commit");
        }
        const ULARGE_INTEGER size{
            .LowPart = information.nFileSizeLow,
            .HighPart = information.nFileSizeHigh};
        if (size.QuadPart != byteSize_) {
            return ioFailure("media identity changed before commit");
        }
        return core::ok();
    }

private:
    std::vector<HANDLE> handles_;
    std::uint64_t byteSize_{0};
};

AppError windowsOpenFailure() {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return {ErrorCode::NotFound, "media file was not found"};
    }
    return ioFailure("media file could not be locked for inspection");
}

Result<LockedMediaPath> lockMediaPath(const ValidatedMediaPath& path,
                                      const std::filesystem::path& relative) {
    LockedMediaPath locked;
    auto current = path.root;
    HANDLE rootHandle = CreateFileW(
        current.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) return windowsOpenFailure();
    locked.add(rootHandle);

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(rootHandle, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
        return ioFailure("media package root could not be inspected");
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return invalid("media package root is redirected");
    }

    for (auto component = relative.begin(); component != relative.end();
         ++component) {
        current /= *component;
        const bool final = std::next(component) == relative.end();
        const DWORD access = final ? GENERIC_READ : FILE_READ_ATTRIBUTES;
        const DWORD sharing = final ? FILE_SHARE_READ
                                    : FILE_SHARE_READ | FILE_SHARE_WRITE;
        const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                            (final ? FILE_FLAG_SEQUENTIAL_SCAN
                                   : FILE_FLAG_BACKUP_SEMANTICS);
        HANDLE handle = CreateFileW(current.c_str(), access, sharing, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return windowsOpenFailure();
        locked.add(handle);

        attributes = {};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                          &attributes, sizeof(attributes))) {
            return ioFailure("media path component could not be inspected");
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return invalid("media path contains a redirected component");
        }
        if (!final &&
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return invalid("media path parent is not a directory");
        }
        if (final) {
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle, &information)) {
                return ioFailure("media file identity could not be inspected");
            }
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return invalid("media path does not name a regular file");
            }
            if (information.nNumberOfLinks != 1) {
                return invalid("media files must not have hard links");
            }
            const ULARGE_INTEGER size{
                .LowPart = information.nFileSizeLow,
                .HighPart = information.nFileSizeHigh};
            if (size.QuadPart == 0) {
                return ioFailure("media file size is invalid");
            }
            locked.setByteSize(size.QuadPart);
        }
    }
    return locked;
}
#else
Result<void> verifyDescriptorInsideRoot(
    int descriptor, const std::filesystem::path& root) {
    std::array<char, PATH_MAX + 1> buffer{};
#ifdef __APPLE__
    if (fcntl(descriptor, F_GETPATH, buffer.data()) != 0) {
        return ioFailure("media file location could not be inspected");
    }
#else
    const std::string descriptorLink =
        "/proc/self/fd/" + std::to_string(descriptor);
    const ssize_t count = readlink(descriptorLink.c_str(), buffer.data(),
                                   buffer.size() - 1);
    if (count < 0 || count == static_cast<ssize_t>(buffer.size() - 1)) {
        return ioFailure("media file location could not be inspected");
    }
    buffer[static_cast<std::size_t>(count)] = '\0';
#endif
    std::error_code error;
    const auto actual =
        std::filesystem::weakly_canonical(buffer.data(), error);
    if (error) {
        return filesystemFailure("media file location could not be resolved",
                                 error);
    }
    if (!isWithin(root, actual)) {
        return invalid("media file descriptor escapes its package root");
    }
    return core::ok();
}

Result<std::string> hashPosixDescriptor(int descriptor,
                                        std::uint64_t byteSize) {
    core::Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t offset = 0;
    while (offset < byteSize) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), byteSize - offset));
        const ssize_t count = pread(descriptor, buffer.data(), requested,
                                    static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            return ioFailure("media file changed while it was read");
        }
        hash.update(std::span<const std::uint8_t>{
            buffer.data(), static_cast<std::size_t>(count)});
        offset += static_cast<std::uint64_t>(count);
    }
    return hash.finish();
}

class LockedMediaPath final : public media::IMediaIdentityLease {
public:
    struct ChangeStamp final {
        std::int64_t modifiedSeconds;
        long modifiedNanoseconds;
        std::int64_t changedSeconds;
        long changedNanoseconds;

        friend bool operator==(const ChangeStamp&, const ChangeStamp&) = default;
    };

    [[nodiscard]] static ChangeStamp changeStamp(
        const struct stat& value) noexcept {
#ifdef __APPLE__
        return ChangeStamp{.modifiedSeconds = value.st_mtimespec.tv_sec,
                           .modifiedNanoseconds = value.st_mtimespec.tv_nsec,
                           .changedSeconds = value.st_ctimespec.tv_sec,
                           .changedNanoseconds = value.st_ctimespec.tv_nsec};
#else
        return ChangeStamp{.modifiedSeconds = value.st_mtim.tv_sec,
                           .modifiedNanoseconds = value.st_mtim.tv_nsec,
                           .changedSeconds = value.st_ctim.tv_sec,
                           .changedNanoseconds = value.st_ctim.tv_nsec};
#endif
    }

    LockedMediaPath(int sourceDescriptor, std::FILE* snapshot,
                    std::uint64_t byteSize, std::filesystem::path root,
                    std::filesystem::path relative, dev_t device, ino_t inode,
                    ChangeStamp changeStamp) noexcept
        : sourceDescriptor_(sourceDescriptor),
          snapshot_(snapshot),
          byteSize_(byteSize),
          root_(std::move(root)),
          relative_(std::move(relative)),
          device_(device),
          inode_(inode),
          changeStamp_(changeStamp) {}
    LockedMediaPath(const LockedMediaPath&) = delete;
    LockedMediaPath& operator=(const LockedMediaPath&) = delete;
    LockedMediaPath(LockedMediaPath&& other) noexcept
        : sourceDescriptor_(std::exchange(other.sourceDescriptor_, -1)),
          snapshot_(std::exchange(other.snapshot_, nullptr)),
          byteSize_(other.byteSize_),
          root_(std::move(other.root_)),
          relative_(std::move(other.relative_)),
          device_(other.device_),
          inode_(other.inode_),
          changeStamp_(other.changeStamp_) {}
    LockedMediaPath& operator=(LockedMediaPath&&) = delete;
    ~LockedMediaPath() {
        if (snapshot_ != nullptr) std::fclose(snapshot_);
        if (sourceDescriptor_ >= 0) close(sourceDescriptor_);
    }

    [[nodiscard]] int descriptor() const noexcept {
        return fileno(snapshot_);
    }
    [[nodiscard]] std::uint64_t byteSize() const noexcept { return byteSize_; }
    [[nodiscard]] Result<void> verifyCurrentIdentity() const override {
        struct stat held {};
        if (sourceDescriptor_ < 0 || fstat(sourceDescriptor_, &held) != 0 ||
            !S_ISREG(held.st_mode) || held.st_nlink != 1 ||
            held.st_dev != device_ || held.st_ino != inode_ ||
            held.st_size <= 0 ||
            static_cast<std::uint64_t>(held.st_size) != byteSize_ ||
            changeStamp(held) != changeStamp_) {
            return ioFailure("media identity changed before commit");
        }
        auto currentPath = validatedMediaPath(root_, relative_);
        if (!currentPath.hasValue()) {
            return ioFailure("media path changed before commit");
        }
        const int current = open(currentPath.value().file.c_str(),
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (current < 0) {
            return ioFailure("media path changed before commit");
        }
        struct stat currentIdentity {};
        const bool matches = fstat(current, &currentIdentity) == 0 &&
                             S_ISREG(currentIdentity.st_mode) &&
                             currentIdentity.st_nlink == 1 &&
                             currentIdentity.st_dev == device_ &&
                             currentIdentity.st_ino == inode_;
        close(current);
        if (!matches) {
            return ioFailure("media path changed before commit");
        }
        return core::ok();
    }

private:
    int sourceDescriptor_{-1};
    std::FILE* snapshot_{nullptr};
    std::uint64_t byteSize_{0};
    std::filesystem::path root_;
    std::filesystem::path relative_;
    dev_t device_{};
    ino_t inode_{};
    ChangeStamp changeStamp_{};
};

AppError posixOpenFailure(std::string action) {
    if (errno == ENOENT) return {ErrorCode::NotFound, std::move(action)};
    if (errno == ELOOP) {
        return invalid("media path contains a redirected component");
    }
    return ioFailure(std::move(action));
}

Result<LockedMediaPath> lockMediaPath(const ValidatedMediaPath& path,
                                      const std::filesystem::path& relative) {
    int directory = open(path.root.c_str(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0) return posixOpenFailure("media package root cannot open");

    for (auto component = relative.begin(); component != relative.end();
         ++component) {
        const bool final = std::next(component) == relative.end();
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                          (final ? 0 : O_DIRECTORY);
        const auto& name = component->native();
        const int opened = openat(directory, name.c_str(), flags);
        const int savedError = errno;
        close(directory);
        directory = -1;
        if (opened < 0) {
            errno = savedError;
            return posixOpenFailure("media path component cannot open");
        }
        if (!final) {
            directory = opened;
            continue;
        }

        struct stat information {};
        if (fstat(opened, &information) != 0) {
            close(opened);
            return ioFailure("media file identity could not be inspected");
        }
        if (!S_ISREG(information.st_mode)) {
            close(opened);
            return invalid("media path does not name a regular file");
        }
        if (information.st_nlink != 1) {
            close(opened);
            return invalid("media files must not have hard links");
        }
        if (information.st_size <= 0) {
            close(opened);
            return ioFailure("media file size is invalid");
        }
        auto containment = verifyDescriptorInsideRoot(opened, path.root);
        if (!containment.hasValue()) {
            const auto failure = containment.error();
            close(opened);
            return failure;
        }
        const auto byteSize = static_cast<std::uint64_t>(information.st_size);
        std::FILE* snapshot = std::tmpfile();
        if (snapshot == nullptr) {
            close(opened);
            return ioFailure("private media snapshot could not be created");
        }
        const int snapshotDescriptor = fileno(snapshot);
        core::Sha256 copiedHash;
        std::array<std::uint8_t, 64 * 1024> buffer{};
        std::uint64_t offset = 0;
        while (offset < byteSize) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), byteSize - offset));
            const ssize_t count = pread(opened, buffer.data(), requested,
                                        static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                std::fclose(snapshot);
                close(opened);
                return ioFailure("media file changed while snapshotting");
            }
            copiedHash.update(std::span<const std::uint8_t>{
                buffer.data(), static_cast<std::size_t>(count)});
            ssize_t written = 0;
            while (written < count) {
                const ssize_t step = pwrite(
                    snapshotDescriptor, buffer.data() + written,
                    static_cast<std::size_t>(count - written),
                    static_cast<off_t>(offset) + written);
                if (step < 0 && errno == EINTR) continue;
                if (step <= 0) {
                    std::fclose(snapshot);
                    close(opened);
                    return ioFailure("private media snapshot could not be written");
                }
                written += step;
            }
            offset += static_cast<std::uint64_t>(count);
        }

        auto sourceHash = hashPosixDescriptor(opened, byteSize);
        const auto snapshotHash = copiedHash.finish();
        struct stat finalInformation {};
        const bool identityValid =
            fstat(opened, &finalInformation) == 0 &&
            S_ISREG(finalInformation.st_mode) && finalInformation.st_nlink == 1 &&
            finalInformation.st_size == information.st_size &&
            finalInformation.st_dev == information.st_dev &&
            finalInformation.st_ino == information.st_ino &&
            LockedMediaPath::changeStamp(finalInformation) ==
                LockedMediaPath::changeStamp(information);
        auto finalContainment = verifyDescriptorInsideRoot(opened, path.root);
        if (!sourceHash.hasValue() || !identityValid ||
            !finalContainment.hasValue() ||
            sourceHash.value() != snapshotHash) {
            std::fclose(snapshot);
            close(opened);
            return ioFailure("media file changed while snapshotting");
        }
        return LockedMediaPath{opened,
                               snapshot,
                               byteSize,
                               path.root,
                               relative,
                               finalInformation.st_dev,
                               finalInformation.st_ino,
                               LockedMediaPath::changeStamp(finalInformation)};
    }
    close(directory);
    return invalid("media path is empty");
}

struct PosixReader final {
    int descriptor{-1};
    std::int64_t byteSize{0};
    std::int64_t position{0};
};

int readPosixFile(void* opaque, std::uint8_t* buffer, int requested) {
    auto& reader = *static_cast<PosixReader*>(opaque);
    if (reader.position >= reader.byteSize) return AVERROR_EOF;
    requested = static_cast<int>(std::min<std::int64_t>(
        requested, reader.byteSize - reader.position));
    for (;;) {
        const ssize_t count = pread(reader.descriptor, buffer,
                                    static_cast<std::size_t>(requested),
                                    reader.position);
        if (count > 0) {
            reader.position += count;
            return static_cast<int>(count);
        }
        if (count == 0) return AVERROR_EOF;
        if (errno != EINTR) return AVERROR(errno);
    }
}

std::int64_t seekPosixFile(void* opaque, std::int64_t offset, int whence) {
    auto& reader = *static_cast<PosixReader*>(opaque);
    if (whence == AVSEEK_SIZE) return reader.byteSize;
    whence &= ~AVSEEK_FORCE;
    std::int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = reader.position;
    } else if (whence == SEEK_END) {
        base = reader.byteSize;
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
        (offset < 0 &&
         base < std::numeric_limits<std::int64_t>::min() - offset)) {
        return AVERROR(EINVAL);
    }
    const auto position = base + offset;
    if (position < 0) return AVERROR(EINVAL);
    reader.position = position;
    return position;
}

struct AvioCloser final {
    void operator()(AVIOContext* context) const noexcept {
        if (context == nullptr) return;
        av_freep(&context->buffer);
        avio_context_free(&context);
    }
};

#endif

#ifdef _WIN32
std::string pathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}
#endif

struct FormatCloser final {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) avformat_close_input(&context);
    }
};

Result<DurationNs> durationOf(const AVFormatContext& context,
                              const std::vector<AVStream*>& streams) {
    constexpr AVRational nanoseconds{1, 1'000'000'000};
    std::int64_t duration = 0;
    if (context.duration > 0 && context.duration != AV_NOPTS_VALUE) {
        duration = av_rescale_q_rnd(
            context.duration, AV_TIME_BASE_Q, nanoseconds,
            static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
        if (duration <= 0 ||
            duration == std::numeric_limits<std::int64_t>::max()) {
            return parseFailure("media duration exceeds supported range");
        }
    }
    for (const auto* stream : streams) {
        if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
            if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
                return parseFailure("media stream time base is invalid");
            }
            const auto streamDuration = av_rescale_q_rnd(
                stream->duration, stream->time_base, nanoseconds,
                static_cast<AVRounding>(AV_ROUND_NEAR_INF |
                                        AV_ROUND_PASS_MINMAX));
            if (streamDuration <= 0 ||
                streamDuration == std::numeric_limits<std::int64_t>::max()) {
                return parseFailure("media duration exceeds supported range");
            }
            duration = std::max(duration, streamDuration);
        }
    }
    if (duration <= 0) {
        return parseFailure("media duration is missing or outside range");
    }
    return DurationNs{duration};
}

}  // namespace

Result<media::MediaProbeResult> FfmpegMediaProbe::probe(
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& relativePath) {
    auto validated = validatedMediaPath(packageRoot, relativePath);
    if (!validated.hasValue()) return validated.error();
    auto locked = lockMediaPath(validated.value(), relativePath);
    if (!locked.hasValue()) return locked.error();
    const auto size = locked.value().byteSize();

    AVFormatContext* opened = nullptr;
#ifdef _WIN32
    const auto path = pathUtf8(validated.value().file);
    if (avformat_open_input(&opened, path.c_str(), nullptr, nullptr) < 0) {
        return parseFailure("media container could not be opened");
    }
#else
    if (size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::int64_t>::max())) {
        return ioFailure("media file size exceeds supported range");
    }
    PosixReader reader{.descriptor = locked.value().descriptor(),
                       .byteSize = static_cast<std::int64_t>(size)};
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(64 * 1024));
    if (ioBuffer == nullptr) {
        return ioFailure("FFmpeg media probe buffer could not be allocated");
    }
    std::unique_ptr<AVIOContext, AvioCloser> io{
        avio_alloc_context(ioBuffer, 64 * 1024, 0, &reader, readPosixFile,
                           nullptr, seekPosixFile)};
    if (io == nullptr) {
        av_free(ioBuffer);
        return ioFailure("FFmpeg media probe IO could not be allocated");
    }
    opened = avformat_alloc_context();
    if (opened == nullptr) {
        return ioFailure("FFmpeg media context could not be allocated");
    }
    opened->pb = io.get();
    opened->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&opened, nullptr, nullptr, nullptr) < 0) {
        if (opened != nullptr) avformat_free_context(opened);
        return parseFailure("media container could not be opened");
    }
#endif
    std::unique_ptr<AVFormatContext, FormatCloser> context{opened};
    if (avformat_find_stream_info(context.get(), nullptr) < 0) {
        return parseFailure("media stream metadata could not be read");
    }

    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    std::vector<AVStream*> primaryStreams;
    for (unsigned int index = 0; index < context->nb_streams; ++index) {
        auto* stream = context->streams[index];
        if (stream == nullptr || stream->codecpar == nullptr) continue;
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (videoStream != nullptr) {
                return parseFailure("media contains multiple video streams");
            }
            videoStream = stream;
            primaryStreams.push_back(stream);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (audioStream != nullptr) {
                return parseFailure("media contains multiple audio streams");
            }
            audioStream = stream;
            primaryStreams.push_back(stream);
        }
    }
    if (primaryStreams.empty()) {
        return parseFailure("media contains no supported stream");
    }

    std::optional<domain::VideoAssetMetadata> video;
    if (videoStream != nullptr) {
        const auto* parameters = videoStream->codecpar;
        if (parameters->width <= 0 || parameters->height <= 0) {
            return parseFailure("video dimensions are invalid");
        }
        const AVRational rate = av_guess_frame_rate(
            context.get(), videoStream, nullptr);
        auto frameRate = core::FrameRate::create(rate.num, rate.den);
        if (!frameRate.hasValue()) {
            return parseFailure("video frame rate is invalid");
        }
        video = domain::VideoAssetMetadata{
            .width = parameters->width,
            .height = parameters->height,
            .frameRate = frameRate.value()};
    }

    std::optional<domain::AudioAssetMetadata> audio;
    if (audioStream != nullptr) {
        const auto* parameters = audioStream->codecpar;
        if (parameters->sample_rate <= 0 ||
            parameters->ch_layout.nb_channels <= 0) {
            return parseFailure("audio metadata is invalid");
        }
        audio = domain::AudioAssetMetadata{
            .sampleRate = parameters->sample_rate,
            .channels = parameters->ch_layout.nb_channels};
    }

    auto duration = durationOf(*context, primaryStreams);
    if (!duration.hasValue()) return duration.error();
#ifdef _WIN32
    auto hash = core::sha256File(validated.value().file);
#else
    auto hash = hashPosixDescriptor(locked.value().descriptor(), size);
#endif
    if (!hash.hasValue()) return hash.error();
    const AVStream* identityStream = videoStream != nullptr ? videoStream : audioStream;
    const char* codec = avcodec_get_name(identityStream->codecpar->codec_id);
    if (codec == nullptr || *codec == '\0' || context->iformat == nullptr ||
        context->iformat->name == nullptr || *context->iformat->name == '\0') {
        return parseFailure("media codec or format identity is missing");
    }

    auto identityLease = std::shared_ptr<const media::IMediaIdentityLease>{
        std::make_shared<LockedMediaPath>(std::move(locked).value())};
    return media::MediaProbeResult{
        .duration = duration.value(),
        .video = std::move(video),
        .audio = std::move(audio),
        .formatName = context->iformat->name,
        .codecName = codec,
        .byteSize = static_cast<std::uint64_t>(size),
        .sha256 = std::move(hash).value(),
        .identityLease = std::move(identityLease)};
}

}  // namespace creator::ffmpeg_adapter
