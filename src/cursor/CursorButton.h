#pragma once

#include "core/Result.h"

#include <string>
#include <string_view>

namespace creator::cursor {

/// The mouse button a click event refers to.
///
/// The set is deliberately the three buttons the telemetry schema
/// (schemas/event.schema.json, cursor.click) admits: left, right, middle. The
/// physical Raw Input adapter that lands later can observe x1/x2 as well, but
/// R2-01's durable click stream only records what the schema and downstream
/// editor understand today.
enum class CursorButton {
    Left,
    Right,
    Middle,
};

/// The canonical schema token for a button. These strings are the enum values
/// in schemas/event.schema.json and must not drift from it.
[[nodiscard]] inline std::string_view toString(CursorButton button) noexcept {
    switch (button) {
        case CursorButton::Left:
            return "left";
        case CursorButton::Right:
            return "right";
        case CursorButton::Middle:
            return "middle";
    }
    return "left";
}

/// Parses a canonical schema token. Fails with InvalidArgument for anything
/// outside the {left, right, middle} allowlist rather than guessing a default,
/// so a malformed persisted stream is rejected instead of silently coerced.
[[nodiscard]] inline core::Result<CursorButton> cursorButtonFromString(std::string_view text) {
    if (text == "left") {
        return CursorButton::Left;
    }
    if (text == "right") {
        return CursorButton::Right;
    }
    if (text == "middle") {
        return CursorButton::Middle;
    }
    return core::AppError{core::ErrorCode::InvalidArgument,
                          "unknown cursor button '" + std::string{text} + "'"};
}

}  // namespace creator::cursor
