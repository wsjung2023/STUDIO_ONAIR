#pragma once

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarMotionSerializer.h"
#include "core/Result.h"

#include <filesystem>

namespace creator::avatar {

/// Appends `avatar.motion` telemetry to a single sequential NDJSON file inside
/// a project package's telemetry directory:
/// `<pkg>/telemetry/avatar-motion.ndjson`.
///
/// This is deliberately NOT the whole-file temp+atomic-rename pattern
/// `project_store::internal::writeFileDurably` uses for the manifest. The
/// manifest is replaced wholesale on every save, so atomic rename is the
/// right shape for it. Telemetry is the opposite: ARCHITECTURE.md §7.4 calls
/// out that "대량 이벤트는 NDJSON으로 순차 기록한다" ("high-volume events are
/// recorded sequentially as NDJSON") — every sample adds exactly one line to
/// the end of a file that keeps growing for the life of the recording.
/// Atomic-rename-per-append would mean rewriting the entire accumulated
/// history on every single sample, which is both wasteful and pointless here:
/// there is no "replace the whole file with a new version" operation to make
/// atomic, only "extend it by one line". So durability instead comes from
/// opening the file in append mode and flushing after every write, which
/// this class does on every call to append().
class AvatarMotionNdjsonSink final {
public:
    /// The fixed filename this sink writes/extends inside `directory`.
    static constexpr const char* kFileName = "avatar-motion.ndjson";

    /// `directory` should already exist — normally the resolved
    /// `<pkg>/telemetry/` directory the project store creates. Constructing
    /// the sink performs no I/O; a directory that turns out to be missing or
    /// unwritable is reported by append(), not here.
    explicit AvatarMotionNdjsonSink(std::filesystem::path directory);

    /// Serializes `sample` (via AvatarMotionSerializer::toNdjsonLine) and
    /// appends the resulting line to `<directory>/avatar-motion.ndjson`,
    /// opening the file in append mode, writing, and flushing before
    /// returning — so a caller that receives ok() knows the line has left
    /// process memory and reached the OS.
    ///
    /// Rejects a sample with `AppError{InvalidArgument}` *before* writing
    /// anything, for either of the two ways an in-range-typed sample can
    /// still produce a schema-invalid document: a negative timestamp, or a
    /// non-finite (NaN/Inf) `parameters` field. The `avatar.motion` schema
    /// (schemas/event.schema.json) requires `tNs >= 0`, and
    /// AvatarMotionSerializer documents that it will not silently clamp a
    /// negative timestamp — doing so would hide a real producer bug
    /// (CLAUDE.md 9). Separately, a NaN/Inf `float` parameter is in-range for
    /// the type but serializes to JSON `null` under nlohmann's default
    /// `dump()`, which violates the schema's `parameters:
    /// additionalProperties {type: number}`. This sink is the boundary where
    /// either invalid document would otherwise reach disk, so this is the
    /// one place both targeted, cheap checks belong: full per-line schema
    /// validation is not needed on this write path because the serializer's
    /// output shape is fixed and these are the only two data-dependent
    /// schema constraints.
    ///
    /// No exception crosses this boundary: filesystem and stream failures
    /// (missing parent directory, path that is a file, permission denial,
    /// disk full, ...) are caught and translated to
    /// `AppError{ErrorCode::IoFailure}`.
    [[nodiscard]] core::Result<void> append(const AvatarMotionSample& sample);

private:
    std::filesystem::path directory_;
    AvatarMotionSerializer serializer_;
};

}  // namespace creator::avatar
