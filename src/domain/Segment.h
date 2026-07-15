#pragma once

#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <string>

namespace creator::domain {

/// Lifecycle of one segment file.
///
/// Ready is only set after the file is closed, fsynced and atomically renamed
/// out of .tmp (ARCHITECTURE.md 6.2). A crash therefore loses at most the
/// segment still in Writing, which is what the "손실 최대 2초" target buys.
enum class SegmentStatus {
    Writing,
    Ready,
    Failed,
};

/// Metadata for one recorded media segment.
///
/// Long recordings are not written as a single MP4: if the app or machine dies
/// the index never gets finalised and the whole take is lost. Short segments
/// plus a project index make recovery possible (ARCHITECTURE.md 6.1).
///
/// This is metadata only. The recorder owns the bytes; the domain owns the
/// index.
struct SegmentInfo final {
    std::uint64_t index{0};
    SourceId sourceId;
    core::TimestampNs startTime{};
    core::DurationNs duration{};
    SegmentStatus status{SegmentStatus::Writing};
    std::string relativePath;

    friend bool operator==(const SegmentInfo&, const SegmentInfo&) = default;
};

}  // namespace creator::domain
