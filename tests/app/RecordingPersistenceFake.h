#pragma once

#include "app/IRecordingPersistence.h"

#include <optional>
#include <string>
#include <utility>

namespace creator::app::test {

class RecordingPersistenceFake final : public IRecordingPersistence {
public:
    void begin(const domain::SessionId& sessionId, core::TimestampNs,
               Completion completion) override {
        beginSessionId_ = sessionId;
        beginCompletion_ = std::move(completion);
    }

    void complete(const domain::RecordingSession& session,
                  Completion completion) override {
        completedSessionId_ = session.id();
        completeCompletion_ = std::move(completion);
    }

    void abort(const domain::SessionId& sessionId, std::string reason,
               Completion completion) override {
        abortedSessionId_ = sessionId;
        abortReason_ = std::move(reason);
        abortCompletion_ = std::move(completion);
    }

    [[nodiscard]] bool beginPending() const noexcept {
        return static_cast<bool>(beginCompletion_);
    }
    [[nodiscard]] bool completePending() const noexcept {
        return static_cast<bool>(completeCompletion_);
    }
    [[nodiscard]] bool abortPending() const noexcept {
        return static_cast<bool>(abortCompletion_);
    }

    void succeedBegin() { finish(beginCompletion_, core::ok()); }
    void failBegin(core::AppError error) {
        finish(beginCompletion_, core::Result<void>{std::move(error)});
    }
    void succeedComplete() { finish(completeCompletion_, core::ok()); }
    void failComplete(core::AppError error) {
        finish(completeCompletion_, core::Result<void>{std::move(error)});
    }
    void succeedAbort() { finish(abortCompletion_, core::ok()); }

private:
    static void finish(Completion& slot, core::Result<void> result) {
        auto completion = std::move(slot);
        slot = {};
        if (completion) completion(std::move(result));
    }

    std::optional<domain::SessionId> beginSessionId_;
    std::optional<domain::SessionId> completedSessionId_;
    std::optional<domain::SessionId> abortedSessionId_;
    std::string abortReason_;
    Completion beginCompletion_;
    Completion completeCompletion_;
    Completion abortCompletion_;
};

}  // namespace creator::app::test
