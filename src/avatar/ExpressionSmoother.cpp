#include "avatar/ExpressionSmoother.h"

#include <chrono>
#include <cmath>
#include <numbers>

namespace creator::avatar {

namespace {

constexpr double kPi = std::numbers::pi;

// alpha(cutoff, dt) from the One-Euro paper: the low-pass blend factor for a
// given cutoff frequency (Hz) and time step (seconds). tau = 1/(2*pi*cutoff)
// is the filter's time constant; alpha -> 1 (no smoothing, follow x exactly)
// as dt grows large relative to tau, and alpha -> 0 (heavy smoothing, stay at
// xPrev) as dt shrinks relative to tau.
double alpha(double cutoffHz, double dt) {
    const double tau = 1.0 / (2.0 * kPi * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

double lowpass(double x, double xPrev, double a) {
    return a * x + (1.0 - a) * xPrev;
}

}  // namespace

float ExpressionSmoother::OneEuroFilter::step(float x, double dt, const Constants& constants) {
    if (!initialized_) {
        // Cold start: no previous sample exists to measure a derivative
        // against, so the documented policy is passthrough - emit x
        // unchanged and seed state from it.
        initialized_ = true;
        xPrev_ = x;
        dxPrev_ = 0.0F;
        return x;
    }
    if (dt <= 0.0) {
        // Repeated or out-of-order timestamp: dt <= 0 would divide by zero
        // or invert time in the derivative below. Per the documented policy,
        // return the last filtered output unchanged and do not advance
        // derivative state - this is a defined degradation, not a crash or
        // a NaN.
        return xPrev_;
    }

    const double xD = static_cast<double>(x);
    const double xPrevD = static_cast<double>(xPrev_);
    const double dxPrevD = static_cast<double>(dxPrev_);

    const double dx = (xD - xPrevD) / dt;
    const double edx = lowpass(dx, dxPrevD, alpha(constants.dCutoffHz, dt));
    const double cutoff = constants.minCutoffHz + constants.beta * std::fabs(edx);
    const double xHat = lowpass(xD, xPrevD, alpha(cutoff, dt));

    xPrev_ = static_cast<float>(xHat);
    dxPrev_ = static_cast<float>(edx);
    return xPrev_;
}

void ExpressionSmoother::OneEuroFilter::reset() {
    initialized_ = false;
    xPrev_ = 0.0F;
    dxPrev_ = 0.0F;
}

ExpressionSmoother::ExpressionSmoother(Constants constants) : constants_(constants) {}

ExpressionParameters ExpressionSmoother::smooth(const ExpressionParameters& raw,
                                                 core::TimestampNs t) {
    // dt comes only from the timestamps the caller supplies - never from a
    // clock read here (CLAUDE.md 9 forbids wall-clock-based sync).
    double dt = 0.0;
    bool advanceTimestamp = true;
    if (haveTimestamp_) {
        dt = std::chrono::duration<double>(t - tPrev_).count();
        advanceTimestamp = dt > 0.0;
    }

    ExpressionParameters out{};
    out.eyeOpenLeft = eyeOpenLeft_.step(raw.eyeOpenLeft, dt, constants_);
    out.eyeOpenRight = eyeOpenRight_.step(raw.eyeOpenRight, dt, constants_);
    out.browUpLeft = browUpLeft_.step(raw.browUpLeft, dt, constants_);
    out.browUpRight = browUpRight_.step(raw.browUpRight, dt, constants_);
    out.mouthOpen = mouthOpen_.step(raw.mouthOpen, dt, constants_);
    out.mouthWide = mouthWide_.step(raw.mouthWide, dt, constants_);
    out.headYaw = headYaw_.step(raw.headYaw, dt, constants_);
    out.headPitch = headPitch_.step(raw.headPitch, dt, constants_);
    out.headRoll = headRoll_.step(raw.headRoll, dt, constants_);

    // Only a positive dt (or the very first call, where dt is irrelevant
    // because every field is still cold-starting) advances the stored
    // timestamp; a rejected non-positive dt leaves tPrev_ where it was so a
    // later, valid timestamp is measured against the last valid one.
    if (advanceTimestamp) {
        tPrev_ = t;
        haveTimestamp_ = true;
    }

    return out;
}

void ExpressionSmoother::reset() {
    haveTimestamp_ = false;
    tPrev_ = core::TimestampNs{};
    eyeOpenLeft_.reset();
    eyeOpenRight_.reset();
    browUpLeft_.reset();
    browUpRight_.reset();
    mouthOpen_.reset();
    mouthWide_.reset();
    headYaw_.reset();
    headPitch_.reset();
    headRoll_.reset();
}

}  // namespace creator::avatar
