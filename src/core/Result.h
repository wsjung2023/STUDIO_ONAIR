#pragma once

#include "core/AppError.h"

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace creator::core {

/// Carries either a value or an AppError.
///
/// std::expected would be the natural fit but it is C++23 and this project is
/// fixed to C++20 (CLAUDE.md 4), so Result<T> is our own.
///
/// [[nodiscard]] is deliberate: silently dropping a Result is how "오류를
/// 무시" (CLAUDE.md 9) happens, and the compiler can catch it for us.
///
/// Calling value() on an error Result is a precondition violation, not a
/// recoverable path — check hasValue() first, or use valueOr().
template <typename T>
class [[nodiscard]] Result final {
public:
    using value_type = T;

    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(AppError error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const T& value() const& {
        assert(hasValue() && "Result::value() called on an error Result");
        return std::get<0>(storage_);
    }

    [[nodiscard]] T& value() & {
        assert(hasValue() && "Result::value() called on an error Result");
        return std::get<0>(storage_);
    }

    [[nodiscard]] T&& value() && {
        assert(hasValue() && "Result::value() called on an error Result");
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] const AppError& error() const& {
        assert(!hasValue() && "Result::error() called on a value Result");
        return std::get<1>(storage_);
    }

    [[nodiscard]] T valueOr(T fallback) const& {
        return hasValue() ? std::get<0>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, AppError> storage_;
};

/// Result specialisation for operations that either succeed or fail without
/// producing a value.
template <>
class [[nodiscard]] Result<void> final {
public:
    using value_type = void;

    Result() = default;
    Result(AppError error) : error_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const AppError& error() const& {
        assert(!hasValue() && "Result::error() called on a value Result");
        return *error_;
    }

private:
    std::optional<AppError> error_;
};

/// Reads better than `return {};` at call sites returning Result<void>.
[[nodiscard]] inline Result<void> ok() { return Result<void>{}; }

}  // namespace creator::core
