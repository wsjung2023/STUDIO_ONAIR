#pragma once

#include "core/Result.h"

#include <chrono>
#include <compare>
#include <string>
#include <string_view>

namespace creator::core {

/// A UTC instant at one second resolution, as written to manifest.json.
///
/// schemas/project.schema.json types createdAt/updatedAt as date-time. Holding
/// them as a value object rather than a bare std::string keeps "매직 문자열
/// 대신 typed ID/value object 사용" (CLAUDE.md 4) honest: an unparseable string
/// cannot reach the manifest, because the only way to build one is through
/// parseRfc3339() or now().
///
/// Formatting and parsing are hand-written. std::chrono::parse is standard
/// C++20, but AppleClang's libc++ support for it lags and macOS is a CI target.
/// The calendar arithmetic used here (year_month_day, hh_mm_ss) is header-only
/// and safe on both toolchains.
///
/// This is a display and metadata type. It must never be used for A/V sync -
/// that is what ProjectClock is for (ARCHITECTURE.md 5.1).
class Utc final {
public:
    [[nodiscard]] static Utc now();

    /// Accepts exactly "YYYY-MM-DDTHH:MM:SSZ". Numeric offsets and fractional
    /// seconds are rejected rather than silently truncated.
    [[nodiscard]] static Result<Utc> parseRfc3339(std::string_view text);

    [[nodiscard]] std::string toRfc3339() const;
    [[nodiscard]] std::chrono::sys_seconds timePoint() const noexcept { return timePoint_; }

    friend bool operator==(const Utc&, const Utc&) = default;
    friend std::strong_ordering operator<=>(const Utc&, const Utc&) = default;

private:
    explicit Utc(std::chrono::sys_seconds timePoint) : timePoint_(timePoint) {}

    std::chrono::sys_seconds timePoint_{};
};

}  // namespace creator::core
