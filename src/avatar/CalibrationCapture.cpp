#include "avatar/CalibrationCapture.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace creator::avatar {

namespace {

bool allFieldsFinite(const ExpressionParameters& p) {
    return std::isfinite(p.eyeOpenLeft) && std::isfinite(p.eyeOpenRight) &&
           std::isfinite(p.browUpLeft) && std::isfinite(p.browUpRight) &&
           std::isfinite(p.mouthOpen) && std::isfinite(p.mouthWide) &&
           std::isfinite(p.headYaw) && std::isfinite(p.headPitch) &&
           std::isfinite(p.headRoll);
}

// Per-field robust baseline: sort this field's accepted values and take the
// middle one. For an even count, average the two middle values (the
// conventional statistics median rule) rather than picking either one
// arbitrarily - both choices are "a middle value", but averaging keeps the
// result stable regardless of which of the two middle frames happened to be
// captured first. A single outlier cannot move this: it can only ever
// occupy one rank-position at one end of the sorted list, so it never
// reaches the middle unless outliers are the majority - unlike a mean, where
// one arbitrarily-large value shifts the result by an amount proportional to
// its own distance from the rest.
float medianOf(std::vector<float> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    const std::size_t mid = n / 2;
    if (n % 2 == 1) {
        return values[mid];
    }
    return (values[mid - 1] + values[mid]) / 2.0F;
}

}  // namespace

void CalibrationCapture::add(const TrackingResult& result) {
    if (!result.faceFound || result.confidence < kMinConfidence ||
        !allFieldsFinite(result.raw)) {
        ++rejectedCount_;
        return;
    }
    accepted_.push_back(result.raw);
}

std::size_t CalibrationCapture::acceptedCount() const {
    return accepted_.size();
}

std::size_t CalibrationCapture::rejectedCount() const {
    return rejectedCount_;
}

core::Result<CalibrationProfile> CalibrationCapture::build() const {
    if (accepted_.size() < kMinCalibrationFrames) {
        return core::AppError(
            core::ErrorCode::InvalidArgument,
            "calibration capture has only " + std::to_string(accepted_.size()) +
                " accepted neutral frame(s), fewer than the required " +
                std::to_string(kMinCalibrationFrames) +
                "; too few for a robust per-field median");
    }

    const std::size_t n = accepted_.size();
    std::vector<float> eyeOpenLeft;
    std::vector<float> eyeOpenRight;
    std::vector<float> browUpLeft;
    std::vector<float> browUpRight;
    std::vector<float> mouthOpen;
    std::vector<float> mouthWide;
    std::vector<float> headYaw;
    std::vector<float> headPitch;
    std::vector<float> headRoll;
    eyeOpenLeft.reserve(n);
    eyeOpenRight.reserve(n);
    browUpLeft.reserve(n);
    browUpRight.reserve(n);
    mouthOpen.reserve(n);
    mouthWide.reserve(n);
    headYaw.reserve(n);
    headPitch.reserve(n);
    headRoll.reserve(n);

    for (const ExpressionParameters& p : accepted_) {
        eyeOpenLeft.push_back(p.eyeOpenLeft);
        eyeOpenRight.push_back(p.eyeOpenRight);
        browUpLeft.push_back(p.browUpLeft);
        browUpRight.push_back(p.browUpRight);
        mouthOpen.push_back(p.mouthOpen);
        mouthWide.push_back(p.mouthWide);
        headYaw.push_back(p.headYaw);
        headPitch.push_back(p.headPitch);
        headRoll.push_back(p.headRoll);
    }

    ExpressionParameters baseline{};
    baseline.eyeOpenLeft = medianOf(std::move(eyeOpenLeft));
    baseline.eyeOpenRight = medianOf(std::move(eyeOpenRight));
    baseline.browUpLeft = medianOf(std::move(browUpLeft));
    baseline.browUpRight = medianOf(std::move(browUpRight));
    baseline.mouthOpen = medianOf(std::move(mouthOpen));
    baseline.mouthWide = medianOf(std::move(mouthWide));
    baseline.headYaw = medianOf(std::move(headYaw));
    baseline.headPitch = medianOf(std::move(headPitch));
    baseline.headRoll = medianOf(std::move(headRoll));

    return CalibrationProfile::fromNeutral(baseline);
}

void CalibrationCapture::reset() {
    accepted_.clear();
    rejectedCount_ = 0;
}

}  // namespace creator::avatar
