#include "fakes/FakeEditEngine.h"

#include "media/MediaTypes.h"

#include <memory>
#include <utility>

namespace creator::fakes {
namespace {

using core::AppError;
using core::DurationNs;
using core::ErrorCode;
using core::Result;
using core::TimestampNs;

class FakeRenderJob final : public edit_engine::IRenderJob {
public:
    explicit FakeRenderJob(DurationNs totalDuration)
        : totalDuration_(totalDuration) {}

    Result<edit_engine::RenderProgress> progress() const override {
        return edit_engine::RenderProgress::create(
            cancelled_ ? edit_engine::RenderJobState::Cancelled
                       : edit_engine::RenderJobState::Pending,
            0.0, TimestampNs{DurationNs::zero()}, totalDuration_);
    }

    Result<void> cancel() override {
        if (cancelled_) {
            return AppError{ErrorCode::InvalidState,
                            "fake render job is already cancelled"};
        }
        cancelled_ = true;
        return core::ok();
    }

private:
    DurationNs totalDuration_;
    bool cancelled_{false};
};

DurationNs timelineDuration(const domain::Timeline& timeline) {
    DurationNs duration{1};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            const auto end = clip.timelineRange().end().time_since_epoch();
            if (end > duration) duration = end;
        }
    }
    return duration;
}

media::VideoFrame frameAt(TimestampNs position) {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 36;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        static_cast<std::size_t>(kWidth) * kHeight * 4U);
    const auto blue = static_cast<std::uint8_t>(
        position.time_since_epoch().count() & 0xff);
    for (std::size_t offset = 0; offset < pixels->size(); offset += 4U) {
        (*pixels)[offset] = blue;
        (*pixels)[offset + 1U] = 0x20;
        (*pixels)[offset + 2U] = 0x30;
        (*pixels)[offset + 3U] = 0xff;
    }
    return media::VideoFrame{
        .timestamp = position,
        .width = kWidth,
        .height = kHeight,
        .visibleRect = {.x = 0, .y = 0, .width = kWidth, .height = kHeight},
        .contentWidth = kWidth,
        .contentHeight = kHeight,
        .contentScale = 1.0,
        .pointPixelScale = 1.0,
        .pixelFormat = media::PixelFormat::Bgra8,
        .colorSpace = media::ColorSpace::Rec709Sdr,
        .platformHandle = std::shared_ptr<void>{pixels, pixels->data()},
    };
}

}  // namespace

Result<void> FakeEditEngine::load(
    const edit_engine::TimelineSnapshot& snapshot) {
    record(FakeEditOperation::Load, snapshot.revision.value());
    if (auto failure = consumeFailure(FakeEditOperation::Load);
        failure.has_value()) {
        return std::move(*failure);
    }
    loaded_ = snapshot;
    playing_ = false;
    playhead_ = TimestampNs{};
    return core::ok();
}

Result<void> FakeEditEngine::update(
    const edit_engine::TimelineChangeSet& change) {
    record(FakeEditOperation::Update, change.target().revision.value());
    if (auto failure = consumeFailure(FakeEditOperation::Update);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    if (loaded_->revision != change.baseRevision() ||
        loaded_->timeline.id() != change.target().timeline.id()) {
        return AppError{ErrorCode::InvalidState,
                        "fake edit engine update is stale or changes timeline identity"};
    }
    loaded_ = change.target();
    return core::ok();
}

Result<void> FakeEditEngine::play() {
    record(FakeEditOperation::Play);
    if (auto failure = consumeFailure(FakeEditOperation::Play);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    playing_ = true;
    return core::ok();
}

Result<void> FakeEditEngine::pause() {
    record(FakeEditOperation::Pause);
    if (auto failure = consumeFailure(FakeEditOperation::Pause);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    playing_ = false;
    return core::ok();
}

Result<void> FakeEditEngine::seek(TimestampNs position) {
    record(FakeEditOperation::Seek, std::nullopt, position);
    if (auto failure = consumeFailure(FakeEditOperation::Seek);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    if (position.time_since_epoch() < DurationNs::zero()) {
        return AppError{ErrorCode::InvalidArgument,
                        "fake edit engine seek must not be negative"};
    }
    playhead_ = position;
    return core::ok();
}

Result<edit_engine::PreviewFrame> FakeEditEngine::requestFrame(
    TimestampNs position) {
    record(FakeEditOperation::RequestFrame, std::nullopt, position);
    if (auto failure = consumeFailure(FakeEditOperation::RequestFrame);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    return edit_engine::PreviewFrame::create(position, loaded_->revision,
                                             frameAt(position));
}

Result<std::unique_ptr<edit_engine::IRenderJob>> FakeEditEngine::render(
    const edit_engine::RenderRequest& request) {
    record(FakeEditOperation::Render, request.snapshot().revision.value());
    if (auto failure = consumeFailure(FakeEditOperation::Render);
        failure.has_value()) {
        return std::move(*failure);
    }
    if (auto ready = requireLoaded(); !ready.hasValue()) return ready.error();
    if (request.snapshot() != *loaded_) {
        return AppError{ErrorCode::InvalidState,
                        "fake render request differs from loaded snapshot"};
    }
    std::unique_ptr<edit_engine::IRenderJob> job =
        std::make_unique<FakeRenderJob>(timelineDuration(loaded_->timeline));
    return job;
}

void FakeEditEngine::failNext(FakeEditOperation operation, AppError error) {
    const auto index = static_cast<std::size_t>(operation);
    if (index < failures_.size()) failures_[index] = std::move(error);
}

std::optional<AppError> FakeEditEngine::consumeFailure(
    FakeEditOperation operation) {
    const auto index = static_cast<std::size_t>(operation);
    if (index >= failures_.size() || !failures_[index].has_value()) {
        return std::nullopt;
    }
    auto failure = std::move(failures_[index]);
    failures_[index].reset();
    return failure;
}

Result<void> FakeEditEngine::requireLoaded() const {
    if (!loaded_.has_value()) {
        return AppError{ErrorCode::InvalidState,
                        "fake edit engine has no loaded timeline"};
    }
    return core::ok();
}

void FakeEditEngine::record(
    FakeEditOperation operation, std::optional<std::int64_t> revision,
    std::optional<TimestampNs> position) {
    calls_.push_back(FakeEditCall{operation, revision, position});
}

}  // namespace creator::fakes
