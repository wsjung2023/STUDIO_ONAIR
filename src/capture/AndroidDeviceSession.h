#pragma once

#include <cstdint>

namespace creator::capture {

enum class DeviceSessionState {
    Idle,
    Starting,
    Streaming,
    Stopping,
    Failed,
    Stopped,
};

/// Qt-free generation gate for Android Camera2 and AudioRecord callbacks.
/// Android owns native resources; this object ensures late callbacks from an
/// older resource set cannot mutate or terminate the current source.
class AndroidDeviceSession final {
public:
    [[nodiscard]] std::uint64_t begin() noexcept;
    [[nodiscard]] bool markStreaming(std::uint64_t generation) noexcept;
    [[nodiscard]] bool acceptsCallbacks(std::uint64_t generation) const noexcept;
    [[nodiscard]] bool fail(std::uint64_t generation) noexcept;
    [[nodiscard]] bool requestStop(std::uint64_t generation) noexcept;
    [[nodiscard]] bool markStopped(std::uint64_t generation) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] DeviceSessionState state() const noexcept { return state_; }

private:
    std::uint64_t generation_{};
    DeviceSessionState state_{DeviceSessionState::Idle};
};

}  // namespace creator::capture
