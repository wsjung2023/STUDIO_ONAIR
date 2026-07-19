#include "app/android/AndroidMediaCodecSegmentEncoder.h"

#include "core/AppError.h"

#include <QJniEnvironment>
#include <QJniObject>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <vector>

namespace creator::app::android {
namespace {

constexpr const char* kActivityClass =
    "com/studioonair/creatorstudio/CreatorStudioActivity";

core::AppError codecError(std::string message) {
    return {core::ErrorCode::IoFailure, std::move(message)};
}

core::Result<void> javaStringResult(QJniObject result,
                                    const char* fallback) {
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions()) return codecError(fallback);
    const auto error = result.toString();
    if (!error.isEmpty()) return codecError(error.toStdString());
    return core::ok();
}

core::Result<void> writeDirect(std::int64_t handle, const void* bytes,
                               std::size_t byteCount, std::int64_t ptsUs,
                               bool video, std::size_t sampleCount = 0) {
    if (!bytes || byteCount == 0 || handle <= 0 ||
        byteCount > static_cast<std::size_t>(std::numeric_limits<jlong>::max())) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec input buffer is invalid"};
    }
    QJniEnvironment environment;
    jobject buffer = environment->NewDirectByteBuffer(
        const_cast<void*>(bytes), static_cast<jlong>(byteCount));
    if (!buffer || environment.checkAndClearExceptions()) {
        return codecError("Android could not allocate a direct codec buffer");
    }
    QJniObject result;
    if (video) {
        result = QJniObject::callStaticObjectMethod(
            kActivityClass, "writeVideoEncoderFrame",
            "(JLjava/nio/ByteBuffer;J)Ljava/lang/String;",
            static_cast<jlong>(handle), buffer, static_cast<jlong>(ptsUs));
    } else {
        if (sampleCount > static_cast<std::size_t>(
                              std::numeric_limits<jint>::max())) {
            environment->DeleteLocalRef(buffer);
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "MediaCodec audio block is too large"};
        }
        result = QJniObject::callStaticObjectMethod(
            kActivityClass, "writeAudioEncoderSamples",
            "(JLjava/nio/ByteBuffer;IJ)Ljava/lang/String;",
            static_cast<jlong>(handle), buffer,
            static_cast<jint>(sampleCount), static_cast<jlong>(ptsUs));
    }
    environment->DeleteLocalRef(buffer);
    return javaStringResult(std::move(result),
                            "Android MediaCodec rejected input");
}

}  // namespace

AndroidMediaCodecSegmentEncoder::AndroidMediaCodecSegmentEncoder(
    recorder::TrackMediaKind mediaKind)
    : mediaKind_(mediaKind) {}

AndroidMediaCodecSegmentEncoder::~AndroidMediaCodecSegmentEncoder() {
    abort();
}

core::Result<void> AndroidMediaCodecSegmentEncoder::start(
    const recorder::SegmentEncodeConfig& config) {
    if (session_.active() || javaHandle_ != 0 || config.partPath.empty() ||
        config.track.mediaKind() != mediaKind_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Android MediaCodec segment cannot start"};
    }
    auto generation = session_.begin(config.startTime);
    if (!generation.hasValue()) return generation.error();
    generation_ = generation.value();
    partPath_ = config.partPath;
    startTime_ = config.startTime;
    lastPresentationTimeUs_ = -1;
    return core::ok();
}

core::Result<void> AndroidMediaCodecSegmentEncoder::ensureVideoCodec(
    std::uint32_t width, std::uint32_t height) {
    if (javaHandle_ != 0) return core::ok();
    if (width > static_cast<std::uint32_t>(std::numeric_limits<jint>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<jint>::max())) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec video dimensions are too large"};
    }
    const auto path = QJniObject::fromString(
        QString::fromStdString(partPath_.generic_string()));
    javaHandle_ = static_cast<std::int64_t>(QJniObject::callStaticMethod<jlong>(
        kActivityClass, "createVideoEncoder", "(Ljava/lang/String;IIII)J",
        path.object<jstring>(), static_cast<jint>(width),
        static_cast<jint>(height), static_cast<jint>(8'000'000),
        static_cast<jint>(30)));
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions() || javaHandle_ <= 0) {
        javaHandle_ = 0;
        return codecError("Android H.264 MediaCodec is unavailable");
    }
    return core::ok();
}

core::Result<void> AndroidMediaCodecSegmentEncoder::ensureAudioCodec(
    std::uint32_t sampleRate, std::uint32_t channels) {
    if (javaHandle_ != 0) return core::ok();
    if (sampleRate > static_cast<std::uint32_t>(std::numeric_limits<jint>::max()) ||
        channels > static_cast<std::uint32_t>(std::numeric_limits<jint>::max())) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MediaCodec audio format is too large"};
    }
    const auto path = QJniObject::fromString(
        QString::fromStdString(partPath_.generic_string()));
    javaHandle_ = static_cast<std::int64_t>(QJniObject::callStaticMethod<jlong>(
        kActivityClass, "createAudioEncoder", "(Ljava/lang/String;III)J",
        path.object<jstring>(), static_cast<jint>(sampleRate),
        static_cast<jint>(channels), static_cast<jint>(192'000)));
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions() || javaHandle_ <= 0) {
        javaHandle_ = 0;
        return codecError("Android AAC MediaCodec is unavailable");
    }
    return core::ok();
}

std::int64_t AndroidMediaCodecSegmentEncoder::presentationTimeUs(
    core::TimestampNs timestamp) noexcept {
    const auto elapsed = timestamp - startTime_;
    const auto requested = elapsed.count() / 1'000;
    lastPresentationTimeUs_ = std::max(requested,
                                       lastPresentationTimeUs_ + 1);
    return lastPresentationTimeUs_;
}

core::Result<void> AndroidMediaCodecSegmentEncoder::accept(
    const media::VideoFrame& frame) {
    if (mediaKind_ != recorder::TrackMediaKind::Video ||
        frame.pixelFormat != media::PixelFormat::Bgra8 ||
        !frame.platformHandle || frame.width == 0 || frame.height == 0) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android video encoder requires a BGRA frame"};
    }
    if (frame.width > std::numeric_limits<std::size_t>::max() / 4U /
                          frame.height) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android video frame size overflows"};
    }
    auto accepted = session_.accept(
        generation_, frame.timestamp,
        AndroidMediaCodecFormat::video(frame.width, frame.height));
    if (!accepted.hasValue()) return accepted.error();
    auto started = ensureVideoCodec(frame.width, frame.height);
    if (!started.hasValue()) return started.error();
    const auto pixels =
        std::static_pointer_cast<std::vector<std::byte>>(frame.platformHandle);
    const auto expected = static_cast<std::size_t>(frame.width) * frame.height * 4U;
    if (!pixels || pixels->size() != expected) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android BGRA frame backing size is invalid"};
    }
    return writeDirect(javaHandle_, pixels->data(), pixels->size(),
                       presentationTimeUs(frame.timestamp), true);
}

core::Result<void> AndroidMediaCodecSegmentEncoder::accept(
    const media::AudioBlock& block) {
    if (mediaKind_ != recorder::TrackMediaKind::Audio || !block.samples ||
        block.sampleRate == 0 || block.channels == 0 || block.frameCount == 0 ||
        !std::isfinite(block.sampleRateRatio) || block.sampleRateRatio < 0.999 ||
        block.sampleRateRatio > 1.001) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android audio encoder input is invalid"};
    }
    auto accepted = session_.accept(
        generation_, block.timestamp,
        AndroidMediaCodecFormat::audio(block.sampleRate, block.channels));
    if (!accepted.hasValue()) return accepted.error();
    auto started = ensureAudioCodec(block.sampleRate, block.channels);
    if (!started.hasValue()) return started.error();

    const auto outputFrames = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::llround(
               static_cast<double>(block.frameCount) * block.sampleRateRatio)));
    if (outputFrames > std::numeric_limits<std::size_t>::max() /
                           block.channels) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Android resampled audio size overflows"};
    }
    std::vector<std::int16_t> pcm(
        static_cast<std::size_t>(outputFrames) * block.channels);
    for (std::uint64_t output = 0; output < outputFrames; ++output) {
        const double sourcePosition =
            static_cast<double>(output) / block.sampleRateRatio;
        const auto left = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(sourcePosition), block.frameCount - 1U);
        const auto right = std::min<std::uint64_t>(left + 1U,
                                                   block.frameCount - 1U);
        const float fraction =
            static_cast<float>(sourcePosition - static_cast<double>(left));
        for (std::uint32_t channel = 0; channel < block.channels; ++channel) {
            const float first = block.samples[left * block.channels + channel];
            const float second = block.samples[right * block.channels + channel];
            const float sample = std::clamp(
                first + (second - first) * fraction, -1.0F, 1.0F);
            pcm[static_cast<std::size_t>(output) * block.channels + channel] =
                static_cast<std::int16_t>(std::lrint(sample * 32'767.0F));
        }
    }
    return writeDirect(javaHandle_, pcm.data(), pcm.size() * sizeof(std::int16_t),
                       presentationTimeUs(block.timestamp), false, pcm.size());
}

core::Result<recorder::EncodedSegment>
AndroidMediaCodecSegmentEncoder::finish(core::TimestampNs endTime) {
    auto finishedState = session_.finish(generation_, endTime);
    if (!finishedState.hasValue()) return finishedState.error();
    if (javaHandle_ <= 0) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Android MediaCodec segment has no native handle"};
    }
    auto result = QJniObject::callStaticObjectMethod(
        kActivityClass, "finishMediaEncoder", "(J)Ljava/lang/String;",
        static_cast<jlong>(javaHandle_));
    javaHandle_ = 0;
    auto completed = javaStringResult(
        std::move(result), "Android MediaCodec could not finish the segment");
    if (!completed.hasValue()) return completed.error();
    std::error_code error;
    const auto bytes = std::filesystem::file_size(partPath_, error);
    if (error || bytes == 0) {
        return codecError("Android MediaMuxer produced an empty segment");
    }
    return recorder::EncodedSegment{
        .endTime = endTime,
        .bytesWritten = bytes,
        .codecName = mediaKind_ == recorder::TrackMediaKind::Video
                         ? "android-mediacodec-h264"
                         : "android-mediacodec-aac"};
}

void AndroidMediaCodecSegmentEncoder::abort() noexcept {
    if (javaHandle_ > 0) {
        QJniObject::callStaticMethod<void>(
            kActivityClass, "abortMediaEncoder", "(J)V",
            static_cast<jlong>(javaHandle_));
        QJniEnvironment environment;
        static_cast<void>(environment.checkAndClearExceptions());
        javaHandle_ = 0;
    }
    session_.abort(generation_);
    std::error_code ignored;
    if (!partPath_.empty()) std::filesystem::remove(partPath_, ignored);
}

core::Result<void> probeAndroidMediaCodec() {
    auto result = QJniObject::callStaticObjectMethod(
        kActivityClass, "mediaEncoderStatus", "()Ljava/lang/String;");
    return javaStringResult(std::move(result),
                            "Android MediaCodec capability probe failed");
}

}  // namespace creator::app::android
