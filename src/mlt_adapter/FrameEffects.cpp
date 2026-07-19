#include "mlt_adapter/FrameEffects.h"

#include "core/AppError.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <numbers>
#include <string>

namespace creator::mlt_adapter {
namespace {

constexpr std::uint32_t kMaximumDimension = 16'384;
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

core::AppError invalid(std::string message) {
    return core::AppError{core::ErrorCode::InvalidArgument, std::move(message)};
}

bool checkedImageBytes(std::uint32_t width, std::uint32_t height,
                       std::uint32_t stride, std::size_t& result) {
    if (width == 0 || height == 0 || width > kMaximumDimension ||
        height > kMaximumDimension) {
        return false;
    }
    const auto rowBytes = static_cast<std::uint64_t>(width) * 4ULL;
    if (stride < rowBytes) return false;
    const auto bytes = static_cast<std::uint64_t>(stride) * (height - 1ULL) +
                       rowBytes;
    if (bytes > std::numeric_limits<std::size_t>::max()) return false;
    result = static_cast<std::size_t>(bytes);
    return true;
}

bool isIdentity(const domain::VisualTransform& transform) noexcept {
    return transform.x() == 0.0 && transform.y() == 0.0 &&
           transform.width() == 1.0 && transform.height() == 1.0 &&
           transform.scaleX() == 1.0 && transform.scaleY() == 1.0 &&
           transform.rotationDegrees() == 0.0 &&
           transform.cropLeft() == 0.0 && transform.cropTop() == 0.0 &&
           transform.cropRight() == 0.0 && transform.cropBottom() == 0.0 &&
           transform.opacity() == 1.0;
}

std::uint8_t interpolateChannel(BgraFrameView source, double sourceX,
                                double sourceY, std::size_t channel) {
    sourceX = std::clamp(sourceX, 0.0,
                         static_cast<double>(source.width - 1U));
    sourceY = std::clamp(sourceY, 0.0,
                         static_cast<double>(source.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(std::floor(sourceX));
    const auto y0 = static_cast<std::uint32_t>(std::floor(sourceY));
    const auto x1 = std::min(x0 + 1U, source.width - 1U);
    const auto y1 = std::min(y0 + 1U, source.height - 1U);
    const double fx = sourceX - x0;
    const double fy = sourceY - y0;
    const auto sample = [&](std::uint32_t x, std::uint32_t y) {
        return static_cast<double>(
            source.bytes[static_cast<std::size_t>(y) * source.stride +
                         static_cast<std::size_t>(x) * 4U + channel]);
    };
    const double top = sample(x0, y0) * (1.0 - fx) + sample(x1, y0) * fx;
    const double bottom = sample(x0, y1) * (1.0 - fx) + sample(x1, y1) * fx;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(top * (1.0 - fy) + bottom * fy), 0L, 255L));
}

std::uint64_t durationToFrames(core::DurationNs duration,
                               std::uint32_t sampleRate) noexcept {
    const auto nanoseconds = static_cast<std::uint64_t>(duration.count());
    const auto seconds = nanoseconds / kNanosecondsPerSecond;
    const auto remainder = nanoseconds % kNanosecondsPerSecond;
    if (seconds > std::numeric_limits<std::uint64_t>::max() / sampleRate) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto whole = seconds * sampleRate;
    const auto fraction = (remainder * sampleRate) / kNanosecondsPerSecond;
    if (whole > std::numeric_limits<std::uint64_t>::max() - fraction) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole + fraction;
}

double rampUp(std::uint64_t position, std::uint64_t frameCount) noexcept {
    if (frameCount == 0 || position >= frameCount) return 1.0;
    if (frameCount == 1) return 0.0;
    return static_cast<double>(position) /
           static_cast<double>(frameCount - 1U);
}

double rampDown(std::uint64_t position, std::uint64_t totalFrames,
                std::uint64_t frameCount) noexcept {
    if (frameCount == 0 || position < totalFrames - frameCount) return 1.0;
    if (frameCount == 1) return 0.0;
    const auto intoFade = position - (totalFrames - frameCount);
    return static_cast<double>(frameCount - 1U - intoFade) /
           static_cast<double>(frameCount - 1U);
}

}  // namespace

ProcessedBgraFrame ProcessedBgraFrame::alias(
    std::span<const std::uint8_t> bytes, std::uint32_t width,
    std::uint32_t height, std::uint32_t stride) {
    return ProcessedBgraFrame{bytes, {}, width, height, stride};
}

ProcessedBgraFrame ProcessedBgraFrame::own(
    std::vector<std::uint8_t> bytes, std::uint32_t width,
    std::uint32_t height, std::uint32_t stride) {
    return ProcessedBgraFrame{{}, std::move(bytes), width, height, stride};
}

std::span<const std::uint8_t> ProcessedBgraFrame::bytes() const noexcept {
    if (owned_.empty()) return borrowed_;
    return owned_;
}

std::span<std::uint8_t> ProcessedBgraFrame::mutableBytes() noexcept {
    return owned_;
}

core::Result<ProcessedBgraFrame> applyVisualTransform(
    BgraFrameView source, std::uint32_t canvasWidth,
    std::uint32_t canvasHeight, const domain::VisualTransform& transform) {
    std::size_t requiredSourceBytes = 0;
    if (!checkedImageBytes(source.width, source.height, source.stride,
                           requiredSourceBytes) ||
        source.bytes.size() < requiredSourceBytes) {
        return invalid("BGRA source buffer geometry is invalid");
    }
    std::size_t outputBytes = 0;
    const auto outputStride64 = static_cast<std::uint64_t>(canvasWidth) * 4ULL;
    if (outputStride64 > std::numeric_limits<std::uint32_t>::max() ||
        !checkedImageBytes(canvasWidth, canvasHeight,
                           static_cast<std::uint32_t>(outputStride64),
                           outputBytes)) {
        return invalid("BGRA canvas geometry is invalid");
    }
    if (source.width == canvasWidth && source.height == canvasHeight &&
        isIdentity(transform)) {
        return ProcessedBgraFrame::alias(source.bytes.first(requiredSourceBytes),
                                         source.width, source.height,
                                         source.stride);
    }

    const double unscaledWidth = transform.width() * canvasWidth;
    const double unscaledHeight = transform.height() * canvasHeight;
    const double destinationWidth = unscaledWidth * transform.scaleX();
    const double destinationHeight = unscaledHeight * transform.scaleY();
    const double centerX = transform.x() * canvasWidth + unscaledWidth * 0.5;
    const double centerY = transform.y() * canvasHeight + unscaledHeight * 0.5;
    const double radians = std::remainder(transform.rotationDegrees(), 360.0) *
                           std::numbers::pi_v<double> / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double croppedWidth =
        (1.0 - transform.cropLeft() - transform.cropRight()) * source.width;
    const double croppedHeight =
        (1.0 - transform.cropTop() - transform.cropBottom()) * source.height;
    if (!std::isfinite(destinationWidth) || destinationWidth <= 0.0 ||
        !std::isfinite(destinationHeight) || destinationHeight <= 0.0 ||
        !std::isfinite(centerX) || !std::isfinite(centerY) ||
        !std::isfinite(croppedWidth) || croppedWidth <= 0.0 ||
        !std::isfinite(croppedHeight) || croppedHeight <= 0.0) {
        return invalid("derived visual transform geometry is invalid");
    }

    std::vector<std::uint8_t> output;
    try {
        output.assign(outputBytes, 0U);
    } catch (const std::bad_alloc&) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "BGRA canvas allocation failed"};
    }
    const auto outputStride = static_cast<std::uint32_t>(outputStride64);

    for (std::uint32_t y = 0; y < canvasHeight; ++y) {
        for (std::uint32_t x = 0; x < canvasWidth; ++x) {
            const double deltaX = static_cast<double>(x) + 0.5 - centerX;
            const double deltaY = static_cast<double>(y) + 0.5 - centerY;
            const double localX = cosine * deltaX + sine * deltaY;
            const double localY = -sine * deltaX + cosine * deltaY;
            const double u = localX / destinationWidth + 0.5;
            const double v = localY / destinationHeight + 0.5;
            if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) continue;

            const double sourceX = transform.cropLeft() * source.width +
                                   u * croppedWidth - 0.5;
            const double sourceY = transform.cropTop() * source.height +
                                   v * croppedHeight - 0.5;
            const auto destination = static_cast<std::size_t>(y) * outputStride +
                                     static_cast<std::size_t>(x) * 4U;
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                const auto sampled =
                    interpolateChannel(source, sourceX, sourceY, channel);
                output[destination + channel] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sampled * transform.opacity()),
                               0L, 255L));
            }
        }
    }
    return ProcessedBgraFrame::own(std::move(output), canvasWidth,
                                   canvasHeight, outputStride);
}

core::Result<void> applyAudioEnvelope(
    std::span<float> interleavedSamples, std::uint32_t channels,
    std::uint64_t firstClipFrame, std::uint64_t totalClipFrames,
    std::uint32_t sampleRate, const domain::AudioEnvelope& envelope) {
    if (channels == 0 || channels > 64U || sampleRate == 0 ||
        sampleRate > 768'000U ||
        interleavedSamples.size() % channels != 0U) {
        return invalid("interleaved audio geometry is invalid");
    }
    const auto bufferFrames = interleavedSamples.size() / channels;
    if (totalClipFrames == 0 || firstClipFrame > totalClipFrames ||
        bufferFrames > totalClipFrames - firstClipFrame) {
        return invalid("audio buffer falls outside its clip");
    }
    const auto fadeInFrames = durationToFrames(envelope.fadeIn(), sampleRate);
    const auto fadeOutFrames = durationToFrames(envelope.fadeOut(), sampleRate);
    if (fadeInFrames > totalClipFrames || fadeOutFrames > totalClipFrames ||
        fadeInFrames > totalClipFrames - fadeOutFrames) {
        return invalid("audio fades exceed the clip frame count");
    }
    if (std::any_of(interleavedSamples.begin(), interleavedSamples.end(),
                    [](float sample) { return !std::isfinite(sample); })) {
        return invalid("audio buffer contains a non-finite sample");
    }

    const double gain = std::pow(10.0, envelope.gainDb() / 20.0);
    for (std::size_t frame = 0; frame < bufferFrames; ++frame) {
        const auto clipFrame = firstClipFrame + frame;
        const double factor = gain * rampUp(clipFrame, fadeInFrames) *
                              rampDown(clipFrame, totalClipFrames,
                                       fadeOutFrames);
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const auto index = frame * channels + channel;
            interleavedSamples[index] = static_cast<float>(std::clamp(
                static_cast<double>(interleavedSamples[index]) * factor,
                -1.0, 1.0));
        }
    }
    return core::ok();
}

}  // namespace creator::mlt_adapter
