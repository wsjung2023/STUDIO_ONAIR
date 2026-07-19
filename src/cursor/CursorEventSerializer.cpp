#include "cursor/CursorEventSerializer.h"

#include <cmath>
#include <string>

namespace creator::cursor {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

[[nodiscard]] Result<void> checkTimestamp(core::TimestampNs tNs) {
    if (tNs.time_since_epoch().count() < 0) {
        return AppError{ErrorCode::InvalidArgument, "telemetry timestamp must not be negative"};
    }
    return core::ok();
}

[[nodiscard]] Result<void> checkPoint(const CursorPoint& point) {
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
        return AppError{ErrorCode::InvalidArgument, "telemetry coordinate must be finite"};
    }
    return core::ok();
}

}  // namespace

Result<nlohmann::json> CursorEventSerializer::toJson(const CursorMoveEvent& event) {
    if (auto ok = checkTimestamp(event.tNs()); !ok) {
        return ok.error();
    }
    if (auto ok = checkPoint(event.point()); !ok) {
        return ok.error();
    }
    // Canonical key order mirrors the schema definition order (base, point,
    // then the type-specific field) for stable, diffable NDJSON lines.
    nlohmann::json json;
    json["tNs"] = event.tNs().time_since_epoch().count();
    json["type"] = "cursor.move";
    json["x"] = event.point().x();
    json["y"] = event.point().y();
    json["sourceId"] = event.sourceId().value();
    return json;
}

Result<nlohmann::json> CursorEventSerializer::toJson(const CursorClickEvent& event) {
    if (auto ok = checkTimestamp(event.tNs()); !ok) {
        return ok.error();
    }
    if (auto ok = checkPoint(event.point()); !ok) {
        return ok.error();
    }
    nlohmann::json json;
    json["tNs"] = event.tNs().time_since_epoch().count();
    json["type"] = "cursor.click";
    json["x"] = event.point().x();
    json["y"] = event.point().y();
    json["button"] = std::string{toString(event.button())};
    return json;
}

}  // namespace creator::cursor
