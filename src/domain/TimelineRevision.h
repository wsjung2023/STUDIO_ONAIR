#pragma once

#include "core/AppError.h"
#include "core/Result.h"

#include <cstdint>
#include <limits>

namespace creator::domain {

class TimelineRevision final {
public:
    [[nodiscard]] static core::Result<TimelineRevision> create(
        std::int64_t value) {
        if (value < 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "timeline revision must not be negative"};
        }
        return TimelineRevision{value};
    }

    [[nodiscard]] std::int64_t value() const noexcept { return value_; }
    [[nodiscard]] core::Result<TimelineRevision> next() const {
        if (value_ == std::numeric_limits<std::int64_t>::max()) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "timeline revision is exhausted"};
        }
        return TimelineRevision{value_ + 1};
    }

    friend bool operator==(const TimelineRevision&,
                           const TimelineRevision&) = default;

private:
    explicit TimelineRevision(std::int64_t value) : value_(value) {}
    std::int64_t value_;
};

}  // namespace creator::domain
