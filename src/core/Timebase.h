#pragma once

#include <chrono>
#include <cstdint>

namespace creator::core {

using Nanoseconds = std::chrono::nanoseconds;
using TimestampNs = std::chrono::duration<std::int64_t, std::nano>;

class MonotonicClock final {
public:
    [[nodiscard]] static TimestampNs now() noexcept {
        return std::chrono::duration_cast<TimestampNs>(
            std::chrono::steady_clock::now().time_since_epoch());
    }
};

}  // namespace creator::core
