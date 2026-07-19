#pragma once

#include "core/Result.h"
#include "cursor/CursorNdjsonSink.h"
#include "cursor/ICursorSource.h"
#include "domain/Identifiers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace creator::cursor {

struct CursorEventPumpStats final {
    std::uint64_t polled{};
    std::uint64_t moves{};
    std::uint64_t clicks{};
    std::uint64_t invalid{};
    std::uint64_t writeFailures{};
};

/// Pulls raw samples from a platform source, normalizes them, and appends the
/// validated cursor events to the durable NDJSON sink. The pump is deliberately
/// synchronous: the application owns its polling cadence and can run it from
/// the same lifecycle worker that owns the recording session.
class CursorEventPump final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<CursorEventPump>> create(
        std::unique_ptr<ICursorSource> source,
        std::unique_ptr<CursorNdjsonSink> sink,
        domain::SourceId sourceId);

    CursorEventPump(const CursorEventPump&) = delete;
    CursorEventPump& operator=(const CursorEventPump&) = delete;
    CursorEventPump(CursorEventPump&&) = delete;
    CursorEventPump& operator=(CursorEventPump&&) = delete;
    ~CursorEventPump() = default;

    /// Drains at most maxSamples. A zero limit is a no-op. Valid samples already
    /// written before an invalid sample or sink failure remain durable.
    [[nodiscard]] core::Result<void> drain(std::size_t maxSamples);

    [[nodiscard]] const CursorEventPumpStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::optional<core::AppError> error() const { return error_; }

private:
    CursorEventPump(std::unique_ptr<ICursorSource> source,
                    std::unique_ptr<CursorNdjsonSink> sink,
                    domain::SourceId sourceId)
        : source_(std::move(source)), sink_(std::move(sink)),
          sourceId_(std::move(sourceId)) {}

    [[nodiscard]] core::Result<void> consume(const RawCursorSample& sample);
    [[nodiscard]] core::Result<void> fail(core::AppError error,
                                          bool writeFailure = false);

    std::unique_ptr<ICursorSource> source_;
    std::unique_ptr<CursorNdjsonSink> sink_;
    domain::SourceId sourceId_;
    CursorEventPumpStats stats_;
    std::optional<core::AppError> error_;
};

}  // namespace creator::cursor

