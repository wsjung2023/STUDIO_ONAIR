#pragma once

#include "core/Result.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "media/IMediaProbe.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace creator::app {

struct RecordingReconcileResult final {
    domain::SessionId sessionId;
    bool imported;
    std::int64_t revision;
    std::size_t assetCount;
    std::size_t trackCount;
    std::size_t markerCount;

    friend bool operator==(const RecordingReconcileResult&,
                           const RecordingReconcileResult&) = default;
};

class IRecordingTimelineReconciler {
public:
    virtual ~IRecordingTimelineReconciler() = default;
    [[nodiscard]] virtual core::Result<RecordingReconcileResult> reconcile(
        const std::filesystem::path& packageRoot,
        const domain::SessionId& sessionId) = 0;
};

class RecordingTimelineReconciler final
    : public IRecordingTimelineReconciler {
public:
    using EventIdFactory = std::function<std::string()>;
    using Clock = std::function<core::Utc()>;

    RecordingTimelineReconciler(media::IMediaProbe& mediaProbe,
                                EventIdFactory eventIdFactory, Clock clock,
                                std::size_t historyLimit = 1'000)
        : mediaProbe_(&mediaProbe),
          eventIdFactory_(std::move(eventIdFactory)),
          clock_(std::move(clock)),
          historyLimit_(historyLimit) {}

    [[nodiscard]] core::Result<RecordingReconcileResult> reconcile(
        const std::filesystem::path& packageRoot,
        const domain::SessionId& sessionId) override;

private:
    media::IMediaProbe* mediaProbe_;
    EventIdFactory eventIdFactory_;
    Clock clock_;
    std::size_t historyLimit_;
};

}  // namespace creator::app
