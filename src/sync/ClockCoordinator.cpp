#include "sync/ClockCoordinator.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace creator::synchronization {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<std::int64_t> checkedAdd(std::int64_t left, std::int64_t right,
                                const char* message) {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right)) {
        return AppError{ErrorCode::InvalidArgument, message};
    }
    return left + right;
}

Result<std::int64_t> checkedSubtract(std::int64_t left, std::int64_t right,
                                     const char* message) {
    if (right == std::numeric_limits<std::int64_t>::min()) {
        if (left >= 0) return AppError{ErrorCode::InvalidArgument, message};
        return left - right;
    }
    return checkedAdd(left, -right, message);
}

Result<std::int64_t> filteredEighth(std::int64_t previous,
                                    std::int64_t current) {
    constexpr std::int64_t denominator = 8;
    const auto previousWhole = previous / denominator;
    const auto currentWhole = current / denominator;
    const auto previousRemainder = previous % denominator;
    const auto currentRemainder = current % denominator;
    const auto base = checkedAdd(previousWhole * 7, currentWhole,
                                 "clock drift filter overflowed");
    if (!base.hasValue()) return base.error();
    const auto remainder =
        (previousRemainder * 7 + currentRemainder) / denominator;
    return checkedAdd(base.value(), remainder, "clock drift filter overflowed");
}

std::int64_t absoluteClamped(std::int64_t value) noexcept {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return value < 0 ? -value : value;
}

}  // namespace

class ClockCoordinator::Impl final {
public:
    struct SourceState final {
        ClockSourceConfig config;
        std::optional<core::TimestampNs> lastSourceTimestamp;
        std::optional<core::TimestampNs> lastObservedAt;
        std::optional<core::TimestampNs> lastEstimatedMaster;
        std::int64_t filteredDriftNs{0};
        std::int64_t maximumAbsoluteDriftNs{0};
        double rateCorrectionPpm{0.0};
        std::uint64_t observationCount{0};
        bool synchronized{false};
    };

    std::vector<SourceState> sources;
    std::size_t masterIndex{0};
    mutable std::mutex mutex;
};

ClockCoordinator::ClockCoordinator(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ClockCoordinator::~ClockCoordinator() = default;

Result<std::unique_ptr<ClockCoordinator>> ClockCoordinator::create(
    std::vector<ClockSourceConfig> sources) {
    if (sources.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        "clock coordinator needs at least one source"};
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto duplicate = std::find_if(
            sources.begin(), sources.begin() + static_cast<std::ptrdiff_t>(index),
            [&sources, index](const auto& candidate) {
                return candidate.sourceId == sources[index].sourceId;
            });
        if (duplicate != sources.begin() + static_cast<std::ptrdiff_t>(index)) {
            return AppError{ErrorCode::AlreadyExists,
                            "clock coordinator source IDs must be unique"};
        }
    }
    const auto master = std::min_element(
        sources.begin(), sources.end(), [](const auto& left, const auto& right) {
            return left.masterPriority < right.masterPriority;
        });
    auto impl = std::make_unique<Impl>();
    impl->masterIndex = static_cast<std::size_t>(master - sources.begin());
    impl->sources.reserve(sources.size());
    for (auto& config : sources) {
        impl->sources.push_back(Impl::SourceState{.config = std::move(config)});
    }
    return std::unique_ptr<ClockCoordinator>{
        new ClockCoordinator{std::move(impl)}};
}

const domain::SourceId& ClockCoordinator::masterSourceId() const noexcept {
    return impl_->sources[impl_->masterIndex].config.sourceId;
}

Result<ClockCorrection> ClockCoordinator::observe(
    const domain::SourceId& sourceId, core::TimestampNs sourceTimestamp,
    core::TimestampNs observedAt) {
    std::lock_guard lock{impl_->mutex};
    const auto found = std::find_if(
        impl_->sources.begin(), impl_->sources.end(),
        [&sourceId](const auto& state) { return state.config.sourceId == sourceId; });
    if (found == impl_->sources.end()) {
        return AppError{ErrorCode::NotFound,
                        "clock observation belongs to an unknown source"};
    }
    auto& state = *found;
    auto next = state;
    if (next.lastSourceTimestamp && sourceTimestamp < *next.lastSourceTimestamp) {
        return AppError{ErrorCode::InvalidArgument,
                        "source timestamp moved backward"};
    }
    if (next.lastObservedAt && observedAt < *next.lastObservedAt) {
        return AppError{ErrorCode::InvalidArgument,
                        "source observation time moved backward"};
    }

    next.lastSourceTimestamp = sourceTimestamp;
    next.lastObservedAt = observedAt;
    ++next.observationCount;
    const auto stateIndex = static_cast<std::size_t>(found - impl_->sources.begin());
    if (stateIndex == impl_->masterIndex) {
        next.filteredDriftNs = 0;
        next.synchronized = true;
        state = std::move(next);
        return ClockCorrection{.correctedTimestamp = sourceTimestamp,
                               .synchronized = true,
                               .master = true};
    }

    const auto& master = impl_->sources[impl_->masterIndex];
    if (!master.lastSourceTimestamp || !master.lastObservedAt) {
        state = std::move(next);
        return ClockCorrection{.correctedTimestamp = sourceTimestamp};
    }
    const auto observedDelta = checkedSubtract(
        observedAt.time_since_epoch().count(),
        master.lastObservedAt->time_since_epoch().count(),
        "clock observation delta overflowed");
    if (!observedDelta.hasValue()) return observedDelta.error();
    const auto estimatedMasterCount = checkedAdd(
        master.lastSourceTimestamp->time_since_epoch().count(), observedDelta.value(),
        "interpolated master timestamp overflowed");
    if (!estimatedMasterCount.hasValue()) return estimatedMasterCount.error();
    const auto rawDrift = checkedSubtract(
        sourceTimestamp.time_since_epoch().count(), estimatedMasterCount.value(),
        "clock drift measurement overflowed");
    if (!rawDrift.hasValue()) return rawDrift.error();

    const std::int64_t previousDrift = next.filteredDriftNs;
    if (!next.synchronized) {
        next.filteredDriftNs = rawDrift.value();
        next.rateCorrectionPpm = 0.0;
        next.synchronized = true;
    } else {
        auto filtered = filteredEighth(next.filteredDriftNs, rawDrift.value());
        if (!filtered.hasValue()) return filtered.error();
        next.filteredDriftNs = filtered.value();
        if (next.lastEstimatedMaster) {
            auto masterElapsed = checkedSubtract(
                estimatedMasterCount.value(),
                next.lastEstimatedMaster->time_since_epoch().count(),
                "master elapsed time overflowed");
            auto driftChange = checkedSubtract(next.filteredDriftNs, previousDrift,
                                               "clock drift slope overflowed");
            if (!masterElapsed.hasValue()) return masterElapsed.error();
            if (!driftChange.hasValue()) return driftChange.error();
            if (masterElapsed.value() > 0) {
                const double observedPpm =
                    static_cast<double>(driftChange.value()) * 1'000'000.0 /
                    static_cast<double>(masterElapsed.value());
                next.rateCorrectionPpm = std::clamp(
                    next.rateCorrectionPpm * 0.875 + observedPpm * 0.125,
                    -1000.0, 1000.0);
            }
        }
    }
    next.lastEstimatedMaster = core::TimestampNs{
        core::ProjectClock::duration{estimatedMasterCount.value()}};
    next.maximumAbsoluteDriftNs = std::max(
        next.maximumAbsoluteDriftNs, absoluteClamped(next.filteredDriftNs));
    const auto correctedCount = checkedSubtract(
        sourceTimestamp.time_since_epoch().count(), next.filteredDriftNs,
        "corrected source timestamp overflowed");
    if (!correctedCount.hasValue()) return correctedCount.error();
    const double ratio = next.config.mediaKind == SyncMediaKind::Audio
                             ? std::clamp(1.0 - next.rateCorrectionPpm / 1'000'000.0,
                                          0.999, 1.001)
                             : 1.0;
    const auto drift = next.filteredDriftNs;
    state = std::move(next);
    return ClockCorrection{
        .correctedTimestamp = core::TimestampNs{
            core::ProjectClock::duration{correctedCount.value()}},
        .drift = core::DurationNs{drift},
        .audioRateRatio = ratio,
        .synchronized = true,
    };
}

ClockCoordinatorSnapshot ClockCoordinator::snapshot() const {
    std::lock_guard lock{impl_->mutex};
    ClockCoordinatorSnapshot result{
        .masterSourceId = impl_->sources[impl_->masterIndex].config.sourceId};
    result.sources.reserve(impl_->sources.size());
    for (std::size_t index = 0; index < impl_->sources.size(); ++index) {
        const auto& state = impl_->sources[index];
        result.sources.push_back(ClockSourceSnapshot{
            .sourceId = state.config.sourceId,
            .drift = core::DurationNs{state.filteredDriftNs},
            .maximumAbsoluteDrift =
                core::DurationNs{state.maximumAbsoluteDriftNs},
            .rateCorrectionPpm = state.rateCorrectionPpm,
            .observationCount = state.observationCount,
            .synchronized = state.synchronized,
            .master = index == impl_->masterIndex,
        });
    }
    return result;
}

}  // namespace creator::synchronization
