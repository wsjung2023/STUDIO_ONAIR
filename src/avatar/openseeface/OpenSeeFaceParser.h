#pragma once

#include "avatar/TrackingResult.h"
#include "core/Result.h"
#include "core/Timebase.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace creator::avatar::openseeface {

/// Size in bytes of one OpenSeeFace face record on the wire, per
/// docs/openseeface-udp-format.md (verified against OpenSeeFace v1.20.4
/// source, not memory). A datagram is N concatenated records of this size.
inline constexpr std::size_t kFaceRecordSizeBytes = 1785;

/// OpenSeeFace's default UDP port (facetracker.py / OpenSee.cs default).
/// Named here, not as a magic literal, even though this header does not open
/// a socket itself (CLAUDE.md 4) — the future UDP source reuses this
/// constant rather than re-declaring the port number.
inline constexpr std::uint16_t kDefaultUdpPort = 11573;

/// Pure decoder: turns one 1785-byte OpenSeeFace face record into a
/// TrackingResult. No socket, no thread, no OpenSeeFace library dependency —
/// this only knows the wire byte layout, verified in
/// docs/openseeface-udp-format.md.
///
/// `projectTime` is supplied by the caller rather than derived from the
/// record's own wire-clock `time` field (offset 0, a Python `time.time()`
/// wall-clock double). CLAUDE.md 9 bans wall-clock A/V sync, and
/// TrackingResult::timestamp is documented as a project-timebase value taken
/// from the source VideoFrame/capture path, never a wall clock. The future
/// OpenSeeFace UDP source is the component that actually knows "what project
/// time is it right now" (it timestamps the datagram at receive time on the
/// same clock the rest of the pipeline uses) — this parser has no such
/// knowledge and must not invent one. The wire `time` field itself is decoded
/// only as a possible future diagnostic/ordering aid; it is not currently
/// surfaced in TrackingResult at all.
///
/// Never throws: a malformed record (wrong size) is reported via
/// `core::AppError{InvalidArgument}`, not an exception, so a future UDP
/// receive loop can treat a bad datagram as a recoverable per-packet error
/// rather than a crash.
///
/// Non-finite (NaN/Inf) feature floats are intentionally NOT rejected here:
/// they are decoded as-is into `TrackingResult::raw` and flow to
/// `ExpressionNormalizer`/`CalibrationProfile::apply`, which already
/// sanitizes a non-finite field to that field's neutral value (Stage A2).
/// Duplicating that sanitization here would create two divergent
/// non-finite-handling paths for the same failure mode.
[[nodiscard]] core::Result<TrackingResult> parseFace(std::span<const std::byte> record,
                                                      core::TimestampNs projectTime);

/// Splits `datagram` into `size() / kFaceRecordSizeBytes` concatenated face
/// records and parses each with `parseFace`, in wire order. Every face in one
/// datagram shares `projectTime`: they were received in the same UDP packet,
/// i.e. the same project-clock instant, so per-face timestamps would not add
/// information the caller doesn't already have.
///
/// Fails with `core::AppError{InvalidArgument}` if `datagram.size()` is not a
/// strictly positive multiple of `kFaceRecordSizeBytes` (a zero-length
/// datagram is rejected too — it carries no face to parse).
[[nodiscard]] core::Result<std::vector<TrackingResult>> parseDatagram(
    std::span<const std::byte> datagram, core::TimestampNs projectTime);

}  // namespace creator::avatar::openseeface
