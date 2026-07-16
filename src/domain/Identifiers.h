#pragma once

#include "core/Result.h"

#include <compare>
#include <string>
#include <utility>

namespace creator::domain {

/// A string identifier that carries its meaning in its type.
///
/// Tag is a phantom parameter: ProjectId and SourceId are separate types even
/// though both wrap a string, so passing one where the other is expected does
/// not compile. This is "매직 문자열 대신 typed ID/value object 사용"
/// (CLAUDE.md 4) made structural.
///
/// The constructor is private and create() is the only way in, so an empty
/// identifier cannot exist. There is deliberately no default constructor: a
/// default-constructed id would be an id with no value, and every consumer
/// would then have to check for that state.
template <typename Tag>
class Identifier final {
public:
    /// Fails with InvalidArgument if value is empty. The string is otherwise
    /// taken as-is: ids come from user-named scenes and sources, so normalising
    /// whitespace or case here would rename what the user typed.
    [[nodiscard]] static core::Result<Identifier> create(std::string value) {
        if (value.empty()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "identifier must not be empty"};
        }
        return Identifier{std::move(value)};
    }

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] std::string toString() const { return value_; }

    friend bool operator==(const Identifier&, const Identifier&) = default;
    friend std::strong_ordering operator<=>(const Identifier&, const Identifier&) = default;

private:
    explicit Identifier(std::string value) noexcept : value_(std::move(value)) {}

    std::string value_;
};

struct ProjectIdTag;
struct SourceIdTag;
struct SessionIdTag;
struct CaptureTargetIdTag;
struct CaptureDeviceIdTag;

using ProjectId = Identifier<ProjectIdTag>;
using SourceId = Identifier<SourceIdTag>;
using SessionId = Identifier<SessionIdTag>;
/// Opaque, session-scoped identity supplied by a screen-capture adapter.
/// It is deliberately distinct from SourceId: a selected OS window may become
/// a project source, but the two lifetimes and persistence rules are different.
using CaptureTargetId = Identifier<CaptureTargetIdTag>;
/// Opaque identity supplied by a native camera/microphone discovery adapter.
/// Device identity is not project source identity and is never inferred from a
/// user-visible device name.
using CaptureDeviceId = Identifier<CaptureDeviceIdTag>;

}  // namespace creator::domain
