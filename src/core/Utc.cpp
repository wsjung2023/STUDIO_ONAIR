#include "core/Utc.h"

#include <array>
#include <cstdint>

namespace creator::core {
namespace {

[[nodiscard]] bool readDigits(std::string_view text, std::size_t offset, std::size_t count,
                              int& out) noexcept {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[offset + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

void appendPadded(std::string& out, int value, int width) {
    std::array<char, 8> buffer{};
    for (int i = width - 1; i >= 0; --i) {
        buffer[static_cast<std::size_t>(i)] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    out.append(buffer.data(), static_cast<std::size_t>(width));
}

}  // namespace

Utc Utc::now() {
    return Utc{std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
}

Result<Utc> Utc::parseRfc3339(std::string_view text) {
    // Exactly "YYYY-MM-DDTHH:MM:SSZ".
    if (text.size() != 20) {
        return AppError{ErrorCode::ParseFailure,
                        "timestamp must be exactly 20 characters of YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != 'Z') {
        return AppError{ErrorCode::ParseFailure,
                        "timestamp must match YYYY-MM-DDTHH:MM:SSZ"};
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!readDigits(text, 0, 4, year) || !readDigits(text, 5, 2, month) ||
        !readDigits(text, 8, 2, day) || !readDigits(text, 11, 2, hour) ||
        !readDigits(text, 14, 2, minute) || !readDigits(text, 17, 2, second)) {
        return AppError{ErrorCode::ParseFailure, "timestamp contains a non-digit"};
    }

    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    // Catches month 13, day 32 and non-days like 2026-02-30 in one check.
    if (!date.ok()) {
        return AppError{ErrorCode::ParseFailure, "timestamp is not a real calendar date"};
    }
    // Leap seconds are not representable in sys_seconds, so 60 is rejected.
    if (hour > 23 || minute > 59 || second > 59) {
        return AppError{ErrorCode::ParseFailure, "timestamp time of day is out of range"};
    }

    const auto days = std::chrono::sys_days{date};
    const auto timeOfDay = std::chrono::hours{hour} + std::chrono::minutes{minute} +
                           std::chrono::seconds{second};
    return Utc{days + timeOfDay};
}

std::string Utc::toRfc3339() const {
    const auto days = std::chrono::floor<std::chrono::days>(timePoint_);
    const std::chrono::year_month_day date{days};
    const std::chrono::hh_mm_ss timeOfDay{timePoint_ - days};

    std::string out;
    out.reserve(20);
    appendPadded(out, static_cast<int>(date.year()), 4);
    out.push_back('-');
    appendPadded(out, static_cast<int>(static_cast<unsigned>(date.month())), 2);
    out.push_back('-');
    appendPadded(out, static_cast<int>(static_cast<unsigned>(date.day())), 2);
    out.push_back('T');
    appendPadded(out, static_cast<int>(timeOfDay.hours().count()), 2);
    out.push_back(':');
    appendPadded(out, static_cast<int>(timeOfDay.minutes().count()), 2);
    out.push_back(':');
    appendPadded(out, static_cast<int>(timeOfDay.seconds().count()), 2);
    out.push_back('Z');
    return out;
}

}  // namespace creator::core
