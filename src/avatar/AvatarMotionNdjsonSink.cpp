#include "avatar/AvatarMotionNdjsonSink.h"

#include "core/AppError.h"

#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

namespace creator::avatar {

using core::AppError;
using core::ErrorCode;
using core::ok;
using core::Result;

AvatarMotionNdjsonSink::AvatarMotionNdjsonSink(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

Result<void> AvatarMotionNdjsonSink::append(const AvatarMotionSample& sample) {
    // Closing Task A3's handoff: the avatar.motion schema requires
    // `tNs >= 0`, but nothing before this boundary enforces it at runtime -
    // AvatarMotionSerializer deliberately serializes a negative timestamp
    // as-is rather than clamping it (CLAUDE.md 9: don't silently paper over a
    // producer bug). This is the write boundary, so it is the one place a
    // schema-invalid line can be stopped before it ever reaches disk. A full
    // per-line schema validation pass is not needed here: the serializer's
    // output shape is fixed, and this plus the non-finite-parameter check
    // below are the only two data-dependent constraints the schema imposes.
    if (sample.timestamp.time_since_epoch().count() < 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "avatar.motion sample has a negative timestamp; "
                        "the event schema requires tNs >= 0"};
    }

    // A NaN/Inf parameter field is in-range for `float` and so passes right
    // through the type system, but nlohmann's default dump() serializes a
    // non-finite float as JSON `null` - which violates the avatar.motion
    // schema's `parameters: additionalProperties {type: number}`. Like the
    // negative-tNs check above, this is the write boundary where such a
    // schema-invalid line would otherwise reach disk, so it is rejected here,
    // before any file touch.
    const std::array<float, 9> parameterFields{
        sample.parameters.eyeOpenLeft,  sample.parameters.eyeOpenRight,
        sample.parameters.browUpLeft,   sample.parameters.browUpRight,
        sample.parameters.mouthOpen,    sample.parameters.mouthWide,
        sample.parameters.headYaw,      sample.parameters.headPitch,
        sample.parameters.headRoll,
    };
    for (const float value : parameterFields) {
        if (!std::isfinite(value)) {
            return AppError{ErrorCode::InvalidArgument,
                            "avatar.motion sample has a non-finite parameter value; "
                            "the event schema requires every parameter to be a JSON number"};
        }
    }

    std::string line;
    try {
        line = serializer_.toNdjsonLine(sample);
    } catch (const std::exception& error) {
        // The serializer only ever emits the fixed set of numeric fields plus
        // the provider id string, so a throw here means the sample's own
        // data is malformed (e.g. a non-UTF-8 provider id) rather than an
        // I/O problem.
        return AppError{ErrorCode::InvalidArgument,
                        std::string{"failed to serialize avatar.motion sample: "} +
                            error.what()};
    }

    try {
        // Built with path::operator/=, not string concatenation: on Windows,
        // std::filesystem::path::string() goes through the process ANSI
        // codepage (CP949 on this machine) and is lossy for non-ASCII path
        // components - that exact bug shipped in R0-01 Task 9 and must not
        // recur here. Appending onto the path object itself stays in the
        // platform-native (wide on Windows) representation the whole way,
        // so a Hangul/emoji telemetry directory round-trips correctly.
        std::filesystem::path filePath = directory_;
        filePath /= kFileName;

        std::ofstream out{filePath, std::ios::binary | std::ios::app};
        if (!out.is_open()) {
            return AppError{ErrorCode::IoFailure,
                            "cannot open avatar-motion.ndjson for append"};
        }
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.flush();
        if (!out) {
            return AppError{ErrorCode::IoFailure,
                            "failed to write/flush avatar-motion.ndjson"};
        }
    } catch (const std::exception& error) {
        // Defensive: std::ofstream does not throw with the default exception
        // mask, but std::filesystem::path construction/append can throw
        // std::system_error for an unrepresentable path. Either way, no
        // exception may cross this boundary (CLAUDE.md 4).
        return AppError{ErrorCode::IoFailure,
                        std::string{"avatar-motion.ndjson append failed: "} + error.what()};
    }

    return ok();
}

}  // namespace creator::avatar
