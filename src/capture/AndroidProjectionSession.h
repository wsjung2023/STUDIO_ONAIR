#pragma once

#include <cstdint>

namespace creator::capture {

/// Lifecycle contract for an Android MediaProjection capture. The JNI bridge
/// owns Android objects; this Qt-free state machine owns only a monotonically
/// increasing generation so late Java callbacks cannot alter a newer capture.
enum class ProjectionSessionState {
    Idle,
    AwaitingApproval,
    Starting,
    Streaming,
    Stopping,
    Revoked,
    Stopped,
};

class AndroidProjectionSession final {
public:
    /// Begins one user-visible Android consent request. The returned generation
    /// is supplied to Java so a late activity result cannot approve a newer
    /// request.
    [[nodiscard]] std::uint64_t beginProjectionRequest() noexcept;
    /// Accepts a matching user approval and allows native capture startup.
    [[nodiscard]] bool approveProjection(std::uint64_t generation) noexcept;
    /// Resolves a matching user denial without leaving a pending session.
    [[nodiscard]] bool denyProjection(std::uint64_t generation) noexcept;
    /// Compatibility helper for callers that already hold an approved Android
    /// projection token and therefore do not need a separate consent phase.
    [[nodiscard]] std::uint64_t beginApprovedProjection() noexcept;
    [[nodiscard]] bool markStreaming(std::uint64_t generation) noexcept;
    /// Bars new frames immediately while Java releases MediaProjection.
    /// Returns true only for the first matching stop request.
    [[nodiscard]] bool requestStop(std::uint64_t generation) noexcept;
    /// Returns true exactly once when the current projection was revoked.
    /// Callers use that transition to stop recorder tracks durably.
    [[nodiscard]] bool onProjectionRevoked(std::uint64_t generation) noexcept;
    [[nodiscard]] bool markStopped(std::uint64_t generation) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] ProjectionSessionState state() const noexcept { return state_; }

private:
    std::uint64_t generation_{0};
    ProjectionSessionState state_{ProjectionSessionState::Idle};
};

}  // namespace creator::capture
