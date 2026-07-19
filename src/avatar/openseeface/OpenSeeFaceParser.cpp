#include "avatar/openseeface/OpenSeeFaceParser.h"

#include "core/AppError.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string>

namespace creator::avatar::openseeface {

namespace {

// Byte offsets into one face record, per docs/openseeface-udp-format.md. Only
// the fields this parser actually maps are named; the rest of the 1785-byte
// record (id, width/height, quaternion, translation, confidence[68],
// points2D[68], points3D[70]) is skipped over by offset arithmetic alone.
constexpr std::size_t kOffsetEyeBlinkRight = 20;   // eye_blink[0]
constexpr std::size_t kOffsetEyeBlinkLeft = 24;    // eye_blink[1]
constexpr std::size_t kOffsetSuccess = 28;
constexpr std::size_t kOffsetPnpError = 29;
constexpr std::size_t kOffsetEulerX = 49;
constexpr std::size_t kOffsetEulerY = 53;
constexpr std::size_t kOffsetEulerZ = 57;
constexpr std::size_t kOffsetEyebrowUpdownL = 1741;  // features[3]
constexpr std::size_t kOffsetEyebrowUpdownR = 1753;  // features[6]
constexpr std::size_t kOffsetMouthOpen = 1777;       // features[12]
constexpr std::size_t kOffsetMouthWide = 1781;       // features[13]

// Head-angle degrees -> [-1,1] normalization scale. Stage-B-tunable: the
// exact value (and the euler-axis -> yaw/pitch/roll assignment used below)
// is a best-effort mapping from docs/openseeface-udp-format.md, not yet
// empirically verified against a live OpenSeeFace feed (raise/lower/turn the
// head and confirm sign + range against this constant at Stage-B
// integration).
constexpr float kMaxHeadAngleDeg = 90.0F;

// euler.z is wire-centered around 90 degrees rather than 0 (see the spec
// doc); this recenters it before normalizing so a camera-facing head reads
// headRoll ~= 0, matching the documented [-1,1] "0 = facing camera" contract.
constexpr float kHeadRollWireCenterDeg = 90.0F;

// OpenSee.cs's maxFit3DError default: the pnp_error scale above which a fit
// is considered worthless. Stage-B-tunable in the same sense as
// kMaxHeadAngleDeg -- copied from the reference implementation's default,
// not independently re-derived.
constexpr float kMaxFitError = 100.0F;

// Explicit little-endian byte assembly, deliberately not a reinterpret_cast
// of a (possibly misaligned) pointer into the buffer -- several field
// offsets in this record are not aligned to their type's size (e.g.
// pnp_error at offset 29), which would be undefined behaviour to read via a
// misaligned float*.
float readFloatLe(std::span<const std::byte> data, std::size_t offset) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < sizeof(float); ++i) {
        bits |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[offset + i]))
                << (8 * i);
    }
    return std::bit_cast<float>(bits);
}

std::uint8_t readUint8(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

float normalizeHeadAngleDeg(float degrees) {
    return std::clamp(degrees / kMaxHeadAngleDeg, -1.0F, 1.0F);
}

}  // namespace

core::Result<TrackingResult> parseFace(std::span<const std::byte> record,
                                        core::TimestampNs projectTime) {
    if (record.size() != kFaceRecordSizeBytes) {
        return core::AppError(
            core::ErrorCode::InvalidArgument,
            "OpenSeeFace face record must be exactly " +
                std::to_string(kFaceRecordSizeBytes) + " bytes, got " +
                std::to_string(record.size()));
    }

    const float eyeBlinkRight = readFloatLe(record, kOffsetEyeBlinkRight);
    const float eyeBlinkLeft = readFloatLe(record, kOffsetEyeBlinkLeft);
    const std::uint8_t success = readUint8(record, kOffsetSuccess);
    const float pnpError = readFloatLe(record, kOffsetPnpError);
    const float eulerX = readFloatLe(record, kOffsetEulerX);
    const float eulerY = readFloatLe(record, kOffsetEulerY);
    const float eulerZ = readFloatLe(record, kOffsetEulerZ);
    const float eyebrowUpdownL = readFloatLe(record, kOffsetEyebrowUpdownL);
    const float eyebrowUpdownR = readFloatLe(record, kOffsetEyebrowUpdownR);
    const float mouthOpen = readFloatLe(record, kOffsetMouthOpen);
    const float mouthWide = readFloatLe(record, kOffsetMouthWide);

    const bool faceFound = success != 0;

    TrackingResult result{};
    result.timestamp = projectTime;
    result.faceFound = faceFound;

    // eye_blink is already [0,1] on the wire -- no scaling needed. Note the
    // wire's eye_blink[0]/[1] index order is right-then-left (see the spec
    // doc), the reverse of this struct's left-then-right field order.
    result.raw.eyeOpenLeft = eyeBlinkLeft;
    result.raw.eyeOpenRight = eyeBlinkRight;

    // Stage-B-tunable: sign polarity of eyebrow up/down is unverified against
    // a live feed (see docs/openseeface-udp-format.md). Passed through
    // un-clamped: OpenSeeFace's features[] block is running-median-normalized
    // rather than hard-clamped, so it can transiently exceed [-1,1] before
    // the calibration window settles -- CalibrationProfile::apply clamps to
    // the documented range downstream, and a non-finite value there is
    // sanitized to that field's neutral value (Stage A2), so this parser
    // does not duplicate either check.
    result.raw.browUpLeft = eyebrowUpdownL;
    result.raw.browUpRight = eyebrowUpdownR;
    result.raw.mouthOpen = mouthOpen;
    result.raw.mouthWide = mouthWide;

    // Stage-B-tunable: euler-axis -> yaw/pitch/roll assignment is unverified
    // against a live feed (see docs/openseeface-udp-format.md).
    result.raw.headYaw = normalizeHeadAngleDeg(eulerY);
    result.raw.headPitch = normalizeHeadAngleDeg(eulerX);
    result.raw.headRoll = normalizeHeadAngleDeg(eulerZ - kHeadRollWireCenterDeg);

    // pnp_error is a fit-error magnitude (large = worse), not a [0,1]
    // confidence -- map it down. A non-finite pnp_error cannot be trusted as
    // a fit-quality measurement, so it is treated the same as "no face
    // found" (confidence 0) rather than propagating NaN into a field
    // documented as [0,1].
    result.confidence = (faceFound && std::isfinite(pnpError))
                             ? std::clamp(1.0F - pnpError / kMaxFitError, 0.0F, 1.0F)
                             : 0.0F;

    return result;
}

core::Result<std::vector<TrackingResult>> parseDatagram(std::span<const std::byte> datagram,
                                                          core::TimestampNs projectTime) {
    if (datagram.empty() || datagram.size() % kFaceRecordSizeBytes != 0) {
        return core::AppError(
            core::ErrorCode::InvalidArgument,
            "OpenSeeFace datagram size must be a positive multiple of " +
                std::to_string(kFaceRecordSizeBytes) + " bytes, got " +
                std::to_string(datagram.size()));
    }

    const std::size_t faceCount = datagram.size() / kFaceRecordSizeBytes;
    std::vector<TrackingResult> results;
    results.reserve(faceCount);
    for (std::size_t i = 0; i < faceCount; ++i) {
        const auto record = datagram.subspan(i * kFaceRecordSizeBytes, kFaceRecordSizeBytes);
        core::Result<TrackingResult> parsed = parseFace(record, projectTime);
        if (!parsed) {
            return parsed.error();
        }
        results.push_back(parsed.value());
    }
    return results;
}

}  // namespace creator::avatar::openseeface
