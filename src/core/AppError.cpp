#include "core/AppError.h"

namespace creator::core {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Unknown:            return "Unknown";
        case ErrorCode::InvalidArgument:    return "InvalidArgument";
        case ErrorCode::NotFound:           return "NotFound";
        case ErrorCode::AlreadyExists:      return "AlreadyExists";
        case ErrorCode::InvalidState:       return "InvalidState";
        case ErrorCode::IoFailure:          return "IoFailure";
        case ErrorCode::ParseFailure:       return "ParseFailure";
        case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    }
    return "Unknown";
}

}  // namespace creator::core
