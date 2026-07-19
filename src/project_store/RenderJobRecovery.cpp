#include "project_store/RenderJobRecovery.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <array>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::project_store {
namespace {

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidState, std::move(message)};
}

core::Result<RenderArtifactEvidence> inspectLocked(
    const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return core::AppError{
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? core::ErrorCode::NotFound
                : core::ErrorCode::IoFailure,
            "render artifact could not be opened"};
    }
    struct HandleCloser final {
        HANDLE value;
        ~HandleCloser() { CloseHandle(value); }
    } closer{handle};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION basic{};
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
        !GetFileInformationByHandle(handle, &basic) ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "render artifact identity could not be read"};
    }
    if ((attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        basic.nNumberOfLinks != 1) {
        return invalid("render artifact must be a regular single-link file");
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(basic.nFileSizeHigh) << 32U) |
        basic.nFileSizeLow;
    std::ostringstream identity;
    identity << "win:" << std::hex << id.VolumeSerialNumber << ':';
    for (const auto value : id.FileId.Identifier) {
        identity << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(value);
    }
    identity << ':' << std::dec << size;
    if (size == 0) return invalid("render artifact is empty");
    core::Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "render artifact could not be hashed"};
        }
        if (count == 0) break;
        hash.update(std::span<const std::uint8_t>{buffer.data(), count});
        total += count;
    }
    if (total != size) {
        return invalid("render artifact changed while it was inspected");
    }
    return RenderArtifactEvidence{size, hash.finish(), identity.str()};
#else
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return core::AppError{errno == ENOENT ? core::ErrorCode::NotFound
                                              : core::ErrorCode::IoFailure,
                              "render artifact could not be opened"};
    }
    struct DescriptorCloser final {
        int value;
        ~DescriptorCloser() { close(value); }
    } closer{descriptor};
    struct stat value {};
    if (fstat(descriptor, &value) != 0) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "render artifact identity could not be read"};
    }
    if (!S_ISREG(value.st_mode) || value.st_nlink != 1) {
        return invalid("render artifact must be a regular single-link file");
    }
    const auto size = static_cast<std::uint64_t>(value.st_size);
    if (size == 0) return invalid("render artifact is empty");
    core::Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "render artifact could not be hashed"};
        }
        if (count == 0) break;
        hash.update(std::span<const std::uint8_t>{
            buffer.data(), static_cast<std::size_t>(count)});
        total += static_cast<std::uint64_t>(count);
    }
    if (total != size) {
        return invalid("render artifact changed while it was inspected");
    }
    return RenderArtifactEvidence{
        size, hash.finish(),
        "posix:" + std::to_string(value.st_dev) + ':' +
            std::to_string(value.st_ino) + ':' + std::to_string(size)};
#endif
}

core::Result<void> advanceTerminal(IRenderJobStore& store,
                                   const RenderJobRecord& record,
                                   edit_engine::RenderJobState state,
                                   core::Utc now, std::string diagnostic) {
    auto progress = edit_engine::RenderProgress::create(
        state, state == edit_engine::RenderJobState::Completed
                   ? 1.0
                   : record.progress.fraction(),
        state == edit_engine::RenderJobState::Completed
            ? core::TimestampNs{record.progress.totalDuration()}
            : record.progress.renderedThrough(),
        record.progress.totalDuration());
    if (!progress.hasValue()) return progress.error();
    auto diagnostics = record.diagnostics;
    diagnostics.diagnostic = diagnostic;
    return store.advance(
        record.jobId,
        {.progress = std::move(progress).value(),
         .diagnostics = std::move(diagnostics),
         .startedAt = record.startedAt,
         .updatedAt = now,
         .finishedAt = now});
}

core::Result<void> renameWithoutReplacement(
    const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "recovery could not publish the matching partial"};
    }
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "recovery could not publish the matching partial"};
    }
#endif
    return core::ok();
}

}  // namespace

core::Result<RenderArtifactEvidence> inspectRenderArtifact(
    const std::filesystem::path& path) {
    return inspectLocked(path);
}

core::Result<std::vector<RenderRecoveryOutcome>> RenderJobRecovery::recoverAll(
    IRenderJobStore& store, core::Utc now) {
    auto records = store.listRecoverable();
    if (!records.hasValue()) return records.error();
    std::vector<RenderRecoveryOutcome> outcomes;
    outcomes.reserve(records.value().size());
    for (const auto& record : records.value()) {
        edit_engine::RenderJobState terminalState =
            edit_engine::RenderJobState::Failed;
        std::string diagnostic = "export was interrupted before publication";
        if (record.progress.state() == edit_engine::RenderJobState::Cancelling) {
            terminalState = edit_engine::RenderJobState::Cancelled;
            diagnostic = "interrupted cancellation was completed";
        } else if (record.progress.state() ==
                   edit_engine::RenderJobState::Publishing) {
            if (!record.diagnostics.outputSha256.has_value() ||
                !record.diagnostics.destinationIdentity.has_value()) {
                diagnostic = "publishing evidence is incomplete";
            } else {
                auto final = inspectRenderArtifact(record.destination);
                if (final.hasValue() &&
                    final.value().sha256 == *record.diagnostics.outputSha256 &&
                    final.value().identity ==
                        *record.diagnostics.destinationIdentity) {
                    terminalState = edit_engine::RenderJobState::Completed;
                    diagnostic = "published artifact was reconciled";
                } else if (!final.hasValue() &&
                           final.error().code() == core::ErrorCode::NotFound) {
                    auto partial = inspectRenderArtifact(record.partial);
                    if (partial.hasValue() &&
                        partial.value().sha256 ==
                            *record.diagnostics.outputSha256 &&
                        partial.value().identity ==
                            *record.diagnostics.destinationIdentity) {
                        auto renamed = renameWithoutReplacement(
                            record.partial, record.destination);
                        if (!renamed.hasValue()) return renamed.error();
                        auto published = inspectRenderArtifact(record.destination);
                        if (published.hasValue() &&
                            published.value() == partial.value()) {
                            terminalState =
                                edit_engine::RenderJobState::Completed;
                            diagnostic =
                                "matching partial was atomically published";
                        } else {
                            diagnostic =
                                "published artifact identity changed during recovery";
                        }
                    } else {
                        diagnostic = "partial artifact did not match publishing evidence";
                    }
                } else {
                    diagnostic = "destination artifact did not match publishing evidence";
                }
            }
        }
        auto advanced = advanceTerminal(store, record, terminalState, now,
                                        diagnostic);
        if (!advanced.hasValue()) return advanced.error();
        outcomes.push_back({record.jobId, terminalState, diagnostic});
    }
    return outcomes;
}

}  // namespace creator::project_store
