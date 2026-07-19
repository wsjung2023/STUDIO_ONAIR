#include "fakes/FakeCursorSource.h"

#include <utility>

namespace creator::fakes {

FakeCursorSource::FakeCursorSource(std::vector<creator::cursor::RawCursorSample> script)
    : script_(std::move(script)) {}

std::optional<creator::cursor::RawCursorSample> FakeCursorSource::poll() {
    if (next_ >= script_.size()) {
        return std::nullopt;
    }
    return script_[next_++];
}

bool FakeCursorSource::exhausted() const noexcept {
    return next_ >= script_.size();
}

}  // namespace creator::fakes
