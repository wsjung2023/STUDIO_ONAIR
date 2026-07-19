#pragma once

#include <cstdint>

namespace creator::capture {

/// Lifecycle contract for an Android MediaProjection capture. The JNI bridge
/// owns Android objects; this Qt-free state machine owns only a monotonically
/// increasing generation so late Java callbacks cannot alter a newer capture.
enum class ProjectionSessionState {
    Idle,
    Starting,
    Streaming,
    Revoked,
    Stopped,
};

class AndroidProjectionSession final {
public:
    [[nodiscard]] std::uint64_t beginApprovedProjection() noexcept;
    [[nodiscard]] bool markStreaming(std::uint64_t generation) noexcept;
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
