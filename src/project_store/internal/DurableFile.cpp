#include "project_store/internal/DurableFile.h"

#include "core/AppError.h"
#include "core/Uuid.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace creator::project_store::internal {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError durableError(std::string_view operation, std::uint64_t code) {
    return AppError{ErrorCode::IoFailure,
                    "durable file " + std::string{operation} + " failed (code " +
                        std::to_string(code) + ")"};
}

std::filesystem::path temporaryPathFor(const std::filesystem::path& target) {
    std::filesystem::path name{"."};
    name += target.filename();
    name += ".part-";
    name += core::generateUuidV4();
    return target.parent_path() / name;
}

void removeTemporary(const std::filesystem::path& temporary) noexcept {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

}  // namespace

Result<void> writeFileDurably(const std::filesystem::path& target,
                              std::string_view contents) {
    std::filesystem::path temporary;
    try {
        temporary = temporaryPathFor(target);
    } catch (const std::system_error&) {
        return AppError{ErrorCode::IoFailure, "durable file temporary path is invalid"};
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return durableError("create temporary", GetLastError());
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto remaining = contents.size() - written;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        const BOOL succeeded =
            WriteFile(handle, contents.data() + written, chunk, &count, nullptr);
        if (!succeeded || count == 0) {
            const DWORD code = succeeded ? ERROR_WRITE_FAULT : GetLastError();
            CloseHandle(handle);
            removeTemporary(temporary);
            return durableError("write temporary", code);
        }
        written += count;
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        removeTemporary(temporary);
        return durableError("flush temporary", code);
    }
    if (!CloseHandle(handle)) {
        const DWORD code = GetLastError();
        removeTemporary(temporary);
        return durableError("close temporary", code);
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        removeTemporary(temporary);
        return durableError("replace target", code);
    }
#else
    int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0) {
        return durableError("create temporary", static_cast<std::uint64_t>(errno));
    }
    std::size_t written = 0;
    while (written < contents.size()) {
        const std::size_t remaining = contents.size() - written;
        const std::size_t chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count = ::write(descriptor, contents.data() + written, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int code = errno;
            ::close(descriptor);
            removeTemporary(temporary);
            return durableError("write temporary", static_cast<std::uint64_t>(code));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const int code = errno;
        ::close(descriptor);
        removeTemporary(temporary);
        return durableError("flush temporary", static_cast<std::uint64_t>(code));
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("close temporary", static_cast<std::uint64_t>(code));
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("replace target", static_cast<std::uint64_t>(code));
    }
    const std::filesystem::path parent = target.parent_path().empty()
                                             ? std::filesystem::path{"."}
                                             : target.parent_path();
    const int parentDescriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (parentDescriptor < 0) {
        return durableError("open parent directory", static_cast<std::uint64_t>(errno));
    }
    if (::fsync(parentDescriptor) != 0) {
        const int code = errno;
        ::close(parentDescriptor);
        return durableError("flush parent directory", static_cast<std::uint64_t>(code));
    }
    if (::close(parentDescriptor) != 0) {
        return durableError("close parent directory", static_cast<std::uint64_t>(errno));
    }
#endif
    return core::ok();
}

}  // namespace creator::project_store::internal
