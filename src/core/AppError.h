#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace creator::core {

/// Product-level error categories. Adapters translate library-specific failures
/// (FFmpeg, MLT, OS capture) into these before they cross a module boundary, so
/// no caller ever has to know which library failed.
enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    InvalidState,
    IoFailure,
    ParseFailure,
    UnsupportedVersion,
    InsufficientStorage,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// An error travelling through Result<T>. Carries a category for programmatic
/// handling and a human-readable message for logs and diagnostics.
///
/// The message must never contain file contents, keystrokes or transcribed
/// speech (ARCHITECTURE.md 11).
class AppError final {
public:
    AppError(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    friend bool operator==(const AppError&, const AppError&) = default;

private:
    ErrorCode code_;
    std::string message_;
};

}  // namespace creator::core
