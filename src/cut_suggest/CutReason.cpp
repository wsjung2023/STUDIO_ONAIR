#include "cut_suggest/CutReason.h"

namespace creator::cut_suggest {

std::string_view toString(CutReason reason) noexcept {
    switch (reason) {
        case CutReason::Silence:
            return "silence";
        case CutReason::Filler:
            return "filler";
    }
    // Unreachable for a validly-constructed enum; kept so the function is total
    // and no compiler warns about a missing return (CLAUDE.md 4).
    return "silence";
}

}  // namespace creator::cut_suggest
