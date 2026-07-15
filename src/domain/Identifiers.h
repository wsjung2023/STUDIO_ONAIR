#pragma once

#include <string>
#include <utility>

namespace creator::domain {

template <typename Tag>
class Identifier final {
public:
    explicit Identifier(std::string value) : value_(std::move(value)) {}

    [[nodiscard]] const std::string& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

    friend bool operator==(const Identifier&, const Identifier&) = default;

private:
    std::string value_;
};

struct ProjectIdTag;
struct SourceIdTag;
struct SessionIdTag;

using ProjectId = Identifier<ProjectIdTag>;
using SourceId = Identifier<SourceIdTag>;
using SessionId = Identifier<SessionIdTag>;

}  // namespace creator::domain
