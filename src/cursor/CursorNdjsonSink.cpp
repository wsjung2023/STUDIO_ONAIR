#include "cursor/CursorNdjsonSink.h"

#include "cursor/CursorEventSerializer.h"

#include <ios>
#include <string>
#include <utility>

namespace creator::cursor {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<std::unique_ptr<CursorNdjsonSink>> CursorNdjsonSink::create(
    const std::filesystem::path& path) {
    // Binary so a '\n' is written as exactly one byte on every platform: the
    // recovery scan counts bytes to the last complete newline, and a translated
    // CRLF would break that arithmetic.
    std::ofstream out(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        return AppError{ErrorCode::IoFailure,
                        "could not open cursor telemetry file for append"};
    }
    // Private ctor: wrap the already-open stream. std::make_unique cannot reach
    // it, so construct explicitly.
    return std::unique_ptr<CursorNdjsonSink>(new CursorNdjsonSink(std::move(out)));
}

Result<void> CursorNdjsonSink::writeLine(const std::string& line) {
    out_ << line << '\n';
    out_.flush();
    if (!out_.good()) {
        return AppError{ErrorCode::IoFailure, "failed to write cursor telemetry line"};
    }
    return core::ok();
}

Result<void> CursorNdjsonSink::write(const CursorMoveEvent& event) {
    auto json = CursorEventSerializer::toJson(event);
    if (!json) {
        return json.error();  // serialize-before-write: nothing hits the file
    }
    return writeLine(json.value().dump());
}

Result<void> CursorNdjsonSink::write(const CursorClickEvent& event) {
    auto json = CursorEventSerializer::toJson(event);
    if (!json) {
        return json.error();
    }
    return writeLine(json.value().dump());
}

}  // namespace creator::cursor
