#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace creator::synchronization {

enum class SyncMediaKind { Video, Audio };

struct ClockSourceConfig final {
    domain::SourceId sourceId;
    SyncMediaKind mediaKind{SyncMediaKind::Video};
    std::uint32_t masterPriority{0};
};

struct ClockCorrection final {
    core::TimestampNs correctedTimestamp{};
    core::DurationNs drift{};
    double audioRateRatio{1.0};
    bool synchronized{false};
    bool master{false};
};

struct ClockSourceSnapshot final {
    domain::SourceId sourceId;
    core::DurationNs drift{};
    core::DurationNs maximumAbsoluteDrift{};
    double rateCorrectionPpm{0.0};
    std::uint64_t observationCount{0};
    bool synchronized{false};
    bool master{false};
};

struct ClockCoordinatorSnapshot final {
    domain::SourceId masterSourceId;
    std::vector<ClockSourceSnapshot> sources;
};

class ClockCoordinator final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<ClockCoordinator>> create(
        std::vector<ClockSourceConfig> sources);

    ClockCoordinator(const ClockCoordinator&) = delete;
    ClockCoordinator& operator=(const ClockCoordinator&) = delete;
    ~ClockCoordinator();

    [[nodiscard]] const domain::SourceId& masterSourceId() const noexcept;
    [[nodiscard]] core::Result<ClockCorrection> observe(
        const domain::SourceId& sourceId, core::TimestampNs sourceTimestamp,
        core::TimestampNs observedAt);
    [[nodiscard]] ClockCoordinatorSnapshot snapshot() const;

private:
    class Impl;
    explicit ClockCoordinator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::synchronization
