#include "mlt_adapter/MltRenderJob.h"

#include "core/AppError.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <utility>

namespace creator::mlt_adapter {
namespace {

core::DurationNs timelineDuration(
    const edit_engine::TimelineSnapshot& snapshot) {
    core::TimestampNs end{};
    for (const auto& track : snapshot.timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end());
        }
    }
    return end.time_since_epoch();
}

bool terminal(edit_engine::RenderJobState state) noexcept {
    return state == edit_engine::RenderJobState::Completed ||
           state == edit_engine::RenderJobState::Failed ||
           state == edit_engine::RenderJobState::Cancelled;
}

}  // namespace

class MltRenderJob::Impl final {
public:
    Impl(edit_engine::RenderRequest request, Operation operation,
         edit_engine::RenderProgress initial)
        : request_(std::move(request)), operation_(std::move(operation)),
          progress_(std::move(initial)) {}

    void start() {
        worker_ = std::jthread{[this](std::stop_token token) { run(token); }};
    }

    void stopAndJoin() {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
    }

    core::Result<edit_engine::RenderProgress> progress() const {
        std::lock_guard lock(mutex_);
        return progress_;
    }

    core::Result<void> cancel() {
        std::lock_guard lock(mutex_);
        if (terminal(progress_.state())) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "render job is already terminal"};
        }
        if (progress_.state() == edit_engine::RenderJobState::Publishing) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                "render publication has crossed the cancellation boundary"};
        }
        worker_.request_stop();
        auto next = edit_engine::RenderProgress::create(
            edit_engine::RenderJobState::Cancelling, progress_.fraction(),
            progress_.renderedThrough(),
            progress_.totalDuration());
        if (!next.hasValue()) return next.error();
        progress_ = std::move(next).value();
        return core::ok();
    }

    std::string diagnostic() const {
        std::lock_guard lock(mutex_);
        return diagnostic_;
    }

private:
    bool publish(edit_engine::RenderJobState state, double fraction,
                 core::TimestampNs renderedThrough) {
        std::lock_guard lock(mutex_);
        if (worker_.get_stop_token().stop_requested() ||
            terminal(progress_.state())) {
            return false;
        }
        auto next = edit_engine::RenderProgress::create(
            state, fraction, renderedThrough, progress_.totalDuration());
        if (!next.hasValue()) {
            diagnostic_ = rejectionDiagnostic(next.error().message(), state,
                                              fraction, renderedThrough);
            return false;
        }
        auto transition =
            edit_engine::validateRenderProgressTransition(progress_, next.value());
        if (!transition.hasValue()) {
            diagnostic_ = rejectionDiagnostic(transition.error().message(), state,
                                              fraction, renderedThrough);
            return false;
        }
        progress_ = std::move(next).value();
        return true;
    }

    std::string rejectionDiagnostic(
        const std::string& reason, edit_engine::RenderJobState nextState,
        double nextFraction, core::TimestampNs nextRenderedThrough) const {
        return "render progress rejected: " + reason +
               " previous_state=" +
               std::to_string(static_cast<int>(progress_.state())) +
               " previous_fraction=" + std::to_string(progress_.fraction()) +
               " previous_rendered_ns=" +
               std::to_string(
                   progress_.renderedThrough().time_since_epoch().count()) +
               " next_state=" + std::to_string(static_cast<int>(nextState)) +
               " next_fraction=" + std::to_string(nextFraction) +
               " next_rendered_ns=" + std::to_string(
                   nextRenderedThrough.time_since_epoch().count());
    }

    void finish(edit_engine::RenderJobState state) {
        std::lock_guard lock(mutex_);
        if (terminal(progress_.state())) return;
        const bool completed = state == edit_engine::RenderJobState::Completed;
        auto next = edit_engine::RenderProgress::create(
            state, completed ? 1.0 : progress_.fraction(),
            completed ? core::TimestampNs{progress_.totalDuration()}
                      : progress_.renderedThrough(),
            progress_.totalDuration());
        if (next.hasValue()) progress_ = std::move(next).value();
    }

    void recordError(const core::Result<void>& result) {
        if (result.hasValue()) return;
        std::lock_guard lock(mutex_);
        if (diagnostic_.empty()) diagnostic_ = result.error().message();
    }

    void run(std::stop_token token) {
        if (!publish(edit_engine::RenderJobState::Running, 0.0,
                     core::TimestampNs{})) {
            // The operation owns external lifecycle finalization. Even when
            // cancellation wins the start race, invoke it once with the
            // already-requested token so persistent state can become
            // Cancelled instead of being left Pending for crash recovery.
            auto result = operation_(
                request_, token,
                [this](edit_engine::RenderJobState state, double fraction,
                       core::TimestampNs renderedThrough) {
                    return publish(state, fraction, renderedThrough);
                });
            recordError(result);
            finish(edit_engine::RenderJobState::Cancelled);
            return;
        }
        auto result = operation_(
            request_, token,
            [this](edit_engine::RenderJobState state, double fraction,
                   core::TimestampNs renderedThrough) {
                return publish(state, fraction, renderedThrough);
            });
        if (token.stop_requested()) {
            recordError(result);
            finish(edit_engine::RenderJobState::Cancelled);
            return;
        }
        if (!result.hasValue()) {
            recordError(result);
            finish(edit_engine::RenderJobState::Failed);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (progress_.state() != edit_engine::RenderJobState::Publishing) {
                auto publishing = edit_engine::RenderProgress::create(
                    edit_engine::RenderJobState::Publishing, 0.999,
                    core::TimestampNs{progress_.totalDuration()},
                    progress_.totalDuration());
                if (publishing.hasValue()) {
                    progress_ = std::move(publishing).value();
                }
            }
        }
        finish(edit_engine::RenderJobState::Completed);
    }

    edit_engine::RenderRequest request_;
    Operation operation_;
    mutable std::mutex mutex_;
    edit_engine::RenderProgress progress_;
    std::string diagnostic_;
    std::jthread worker_;
};

core::Result<std::unique_ptr<edit_engine::IRenderJob>> MltRenderJob::start(
    edit_engine::RenderRequest request, Operation operation) {
    if (!operation) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "render operation is required"};
    }
    auto initial = edit_engine::RenderProgress::create(
        edit_engine::RenderJobState::Pending, 0.0, core::TimestampNs{},
        timelineDuration(request.snapshot()));
    if (!initial.hasValue()) return initial.error();
    auto concrete = std::unique_ptr<MltRenderJob>{new MltRenderJob{
        std::make_unique<Impl>(std::move(request), std::move(operation),
                               std::move(initial).value())}};
    concrete->impl_->start();
    std::unique_ptr<edit_engine::IRenderJob> result = std::move(concrete);
    return result;
}

MltRenderJob::MltRenderJob(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MltRenderJob::~MltRenderJob() { impl_->stopAndJoin(); }

core::Result<edit_engine::RenderProgress> MltRenderJob::progress() const {
    return impl_->progress();
}

core::Result<void> MltRenderJob::cancel() { return impl_->cancel(); }

std::string MltRenderJob::diagnostic() const { return impl_->diagnostic(); }

}  // namespace creator::mlt_adapter
