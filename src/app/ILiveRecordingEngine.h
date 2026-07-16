#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/RecordingSession.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace creator::app {

struct LiveRecordingStart final {
    domain::SessionId sessionId;
    std::filesystem::path packagePath;
    core::TimestampNs startedAt{};
};

struct LiveRecordingEngineSnapshot final {
    std::size_t trackCount{0};
    std::uint64_t queuedItems{0};
    std::uint64_t videoFramesDropped{0};
    std::uint64_t segmentsPublished{0};
    std::optional<std::uint64_t> availableDiskBytes;
    std::string encoderName;
    std::optional<core::AppError> terminalError;
};

/// A stopped live take. A terminal capture/encode error is carried alongside
/// the stopped session so already-published READY segments can still be
/// committed instead of being discarded or left as a false crash recovery.
struct LiveRecordingCompletion final {
    domain::RecordingSession session;
    std::size_t trackCount{0};
    std::uint64_t segmentsPublished{0};
    std::uint64_t videoFramesDropped{0};
    std::optional<core::AppError> terminalError;
};

class ILiveRecordingEngine {
public:
    using Completion =
        std::function<void(core::Result<LiveRecordingCompletion>)>;

    virtual ~ILiveRecordingEngine() = default;
    ILiveRecordingEngine(const ILiveRecordingEngine&) = delete;
    ILiveRecordingEngine& operator=(const ILiveRecordingEngine&) = delete;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual std::string unavailableReason() const = 0;
    [[nodiscard]] virtual core::Result<void> start(
        LiveRecordingStart start, Completion completion) = 0;
    virtual void stopAsync(core::TimestampNs stoppedAt) = 0;
    [[nodiscard]] virtual LiveRecordingEngineSnapshot snapshot() const = 0;

protected:
    ILiveRecordingEngine() = default;
};

}  // namespace creator::app
