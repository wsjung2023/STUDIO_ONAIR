#include "sync/VideoSyncPlanner.h"

#include "core/AppError.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace creator::synchronization {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<core::TimestampNs> addChecked(core::TimestampNs timestamp,
                                     core::DurationNs duration) {
    const auto left = timestamp.time_since_epoch().count();
    const auto right = duration.count();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if ((right > 0 && left > maximum - right) ||
        (right < 0 && left < minimum - right)) {
        return AppError{ErrorCode::InvalidArgument,
                        "video synchronization timestamp overflowed"};
    }
    return core::TimestampNs{core::ProjectClock::duration{left + right}};
}

Result<std::int64_t> nonnegativeDifference(core::TimestampNs newer,
                                           core::TimestampNs older) {
    const auto left = newer.time_since_epoch().count();
    const auto right = older.time_since_epoch().count();
    if (left < right) {
        return AppError{ErrorCode::InvalidArgument,
                        "video synchronization timestamps are out of order"};
    }
    if (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) {
        return AppError{ErrorCode::InvalidArgument,
                        "video synchronization gap overflowed"};
    }
    return left - right;
}

void incrementSaturated(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    const auto remaining = std::numeric_limits<std::uint64_t>::max() - value;
    value += std::min(remaining, amount);
}

}  // namespace

class VideoSyncPlanner::Impl final {
public:
    core::DurationNs period{};
    core::DurationNs halfPeriod{};
    std::size_t maximumDuplicatesPerInput{0};
    std::optional<core::TimestampNs> nextTimestamp;
    std::optional<media::VideoFrame> lastFrame;
    VideoSyncSnapshot snapshot;
};

VideoSyncPlanner::VideoSyncPlanner(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

VideoSyncPlanner::~VideoSyncPlanner() = default;

Result<std::unique_ptr<VideoSyncPlanner>> VideoSyncPlanner::create(
    core::DurationNs framePeriod, std::size_t maximumDuplicatesPerInput) {
    if (framePeriod <= core::DurationNs::zero()) {
        return AppError{ErrorCode::InvalidArgument,
                        "video synchronization frame period must be positive"};
    }
    if (maximumDuplicatesPerInput == 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "video synchronization duplicate bound must be positive"};
    }
    auto impl = std::make_unique<Impl>();
    impl->period = framePeriod;
    impl->halfPeriod = framePeriod / 2;
    impl->maximumDuplicatesPerInput = maximumDuplicatesPerInput;
    return std::unique_ptr<VideoSyncPlanner>{
        new VideoSyncPlanner{std::move(impl)}};
}

Result<VideoSyncBatch> VideoSyncPlanner::plan(
    media::VideoFrame frame, core::TimestampNs correctedTimestamp) {
    auto state = *impl_;
    VideoSyncBatch result;
    if (!state.nextTimestamp) {
        frame.timestamp = correctedTimestamp;
        auto next = addChecked(correctedTimestamp, state.period);
        if (!next.hasValue()) return next.error();
        state.nextTimestamp = next.value();
        state.lastFrame = frame;
        result.frames.push_back(std::move(frame));
        incrementSaturated(state.snapshot.framesPassed);
        *impl_ = std::move(state);
        return result;
    }

    auto lateBoundary = addChecked(*state.nextTimestamp, -state.halfPeriod);
    if (!lateBoundary.hasValue()) return lateBoundary.error();
    if (correctedTimestamp < lateBoundary.value()) {
        incrementSaturated(state.snapshot.framesDropped);
        *impl_ = std::move(state);
        return result;
    }

    std::size_t duplicates = 0;
    while (duplicates < state.maximumDuplicatesPerInput) {
        auto earlyBoundary = addChecked(*state.nextTimestamp, state.halfPeriod);
        if (!earlyBoundary.hasValue()) return earlyBoundary.error();
        if (correctedTimestamp <= earlyBoundary.value()) break;
        auto duplicate = *state.lastFrame;
        duplicate.timestamp = *state.nextTimestamp;
        result.frames.push_back(std::move(duplicate));
        auto next = addChecked(*state.nextTimestamp, state.period);
        if (!next.hasValue()) return next.error();
        state.nextTimestamp = next.value();
        ++duplicates;
        incrementSaturated(state.snapshot.framesDuplicated);
    }

    auto earlyBoundary = addChecked(*state.nextTimestamp, state.halfPeriod);
    if (!earlyBoundary.hasValue()) return earlyBoundary.error();
    if (correctedTimestamp > earlyBoundary.value()) {
        const auto difference =
            nonnegativeDifference(correctedTimestamp, *state.nextTimestamp);
        if (!difference.hasValue()) return difference.error();
        const auto skipped = static_cast<std::uint64_t>(
            difference.value() / state.period.count());
        const auto jump = core::DurationNs{
            static_cast<std::int64_t>(skipped) * state.period.count()};
        auto next = addChecked(*state.nextTimestamp, jump);
        if (!next.hasValue()) return next.error();
        state.nextTimestamp = next.value();
        incrementSaturated(state.snapshot.gridIntervalsSkipped, skipped);
    }

    frame.timestamp = *state.nextTimestamp;
    auto next = addChecked(*state.nextTimestamp, state.period);
    if (!next.hasValue()) return next.error();
    state.nextTimestamp = next.value();
    state.lastFrame = frame;
    result.frames.push_back(std::move(frame));
    incrementSaturated(state.snapshot.framesPassed);
    *impl_ = std::move(state);
    return result;
}

VideoSyncSnapshot VideoSyncPlanner::snapshot() const noexcept {
    return impl_->snapshot;
}

}  // namespace creator::synchronization
