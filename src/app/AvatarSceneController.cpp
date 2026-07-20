#include "app/AvatarSceneController.h"

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarRenderFrame.h"
#include "core/AppError.h"

#if defined(CS_APP_ENABLE_FFMPEG)
#include "ffmpeg_adapter/BgraFrameMappers.h"
#endif

#include <algorithm>
#include <cstring>
#include <utility>

namespace creator::app {

AvatarSceneController::AvatarSceneController(
    std::unique_ptr<avatar::ITrackingProvider> provider,
    avatar::AvatarParameterMapper mapper,
    std::unique_ptr<avatar::IAvatarRenderer> renderer, std::uint32_t width,
    std::uint32_t height, std::function<bool()> cameraLive, bool usingRealModel,
    bool usingRealTracking, QObject* parent)
    : QObject(parent),
      provider_(std::move(provider)),
      mapper_(std::move(mapper)),
      renderer_(std::move(renderer)),
      pipeline_(mapper_, *renderer_),
      width_(width),
      height_(height),
      cameraLive_(std::move(cameraLive)),
      sourceId_(creator::domain::SourceId::create("avatar").value()) {
    const QString trackingKind = usingRealTracking
                                     ? tr("live face tracking")
                                     : tr("synthetic (fake) tracking");
    const QString modelKind = usingRealModel ? tr("Inochi2D model")
                                             : tr("placeholder avatar");
    trackingLabel_ = tr("%1 · %2").arg(trackingKind, modelKind);
    status_ = tr("Avatar idle");
    timer_.setInterval(33);  // ~30 fps
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

AvatarSceneController::~AvatarSceneController() { timer_.stop(); }

void AvatarSceneController::setAvatarRecordingSink(
    std::shared_ptr<creator::capture::IVideoFrameSink> sink) noexcept {
    recordingSink_ = std::move(sink);
    if (fanout_) fanout_->setSecondary(recordingSink_);
}

void AvatarSceneController::setAvatarEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (enabled_) {
        previewMailbox_ =
            std::make_shared<creator::capture::LatestVideoFrameMailbox>();
        fanout_ = std::make_shared<creator::capture::VideoFrameFanoutSink>(
            previewMailbox_);
        fanout_->setSecondary(recordingSink_);
        fanout_->onCaptureStarted();
        animationPhase_ = creator::core::DurationNs{0};
        lastTick_.reset();
        capturing_ = true;
        publishStatus(tr("Avatar live — %1").arg(trackingLabel_));
        timer_.start();
    } else {
        timer_.stop();
        capturing_ = false;
        fanout_.reset();
        previewMailbox_.reset();
        publishStatus(tr("Avatar stopped"));
    }
    emit stateChanged();
}

void AvatarSceneController::publishStatus(QString message) {
    status_ = std::move(message);
}

QImage AvatarSceneController::renderDiagnosticImage(double extraSeconds) {
    const auto extra = std::chrono::duration_cast<creator::core::DurationNs>(
        std::chrono::duration<double>(extraSeconds));
    creator::media::VideoFrame phaseFrame{};
    phaseFrame.timestamp = creator::core::TimestampNs{} + animationPhase_ + extra;
    phaseFrame.width = width_;
    phaseFrame.height = height_;
    phaseFrame.pixelFormat = creator::media::PixelFormat::Bgra8;
    auto tracked = provider_->process(phaseFrame);
    if (!tracked.hasValue()) return {};
    const avatar::AvatarMotionSample sample{
        .timestamp = phaseFrame.timestamp,
        .parameters = tracked.value().raw,
        .provider = provider_->providerId(),
    };
    auto rendered = pipeline_.render(sample);
    if (!rendered.hasValue()) return {};
    const auto& frame = rendered.value();
    const auto bytes = frame.bytes();
    // Format_ARGB32 memory layout is B,G,R,A little-endian, matching packed BGRA.
    const QImage view{bytes.data(), static_cast<int>(frame.width()),
                      static_cast<int>(frame.height()),
                      static_cast<int>(frame.stride()), QImage::Format_ARGB32};
    return view.copy();
}

void AvatarSceneController::tick() {
    if (!enabled_ || !fanout_) return;

    const auto now = creator::core::ProjectClock::now();
    const bool live = cameraLive_ ? cameraLive_() : true;
    // Advance the synthetic expression's phase only while the webcam is
    // capturing, so the camera's presence drives the avatar's motion. A frozen
    // phase holds the last pose when the camera is not live.
    if (lastTick_.has_value() && live) {
        animationPhase_ += now - *lastTick_;
    }
    lastTick_ = now;

    // The provider reads only the frame timestamp (it ignores pixels); feed it
    // the small, camera-gated phase so the animation is well-conditioned.
    creator::media::VideoFrame phaseFrame{};
    phaseFrame.timestamp = creator::core::TimestampNs{} + animationPhase_;
    phaseFrame.width = width_;
    phaseFrame.height = height_;
    phaseFrame.pixelFormat = creator::media::PixelFormat::Bgra8;

    auto tracked = provider_->process(phaseFrame);
    if (!tracked.hasValue()) {
        publishStatus(tr("Avatar tracking error: %1")
                          .arg(QString::fromStdString(tracked.error().message())));
        emit stateChanged();
        return;
    }
    const avatar::AvatarMotionSample sample{
        .timestamp = phaseFrame.timestamp,
        .parameters = tracked.value().raw,
        .provider = provider_->providerId(),
    };
    auto rendered = pipeline_.render(sample);
    if (!rendered.hasValue()) {
        publishStatus(tr("Avatar render error: %1")
                          .arg(QString::fromStdString(rendered.error().message())));
        emit stateChanged();
        return;
    }
    const auto& frame = rendered.value();
    const auto bytes = frame.bytes();

#if defined(CS_APP_ENABLE_FFMPEG)
    // Wrap the packed BGRA in the CpuBgraFrameBuffer the preview node and the
    // recording encoder both expect (mirrors the camera's Windows frame handle).
    auto buffer = creator::ffmpeg_adapter::CpuBgraFrameBuffer::create(
        frame.width(), frame.height());
    if (!buffer.hasValue()) {
        publishStatus(tr("Avatar frame buffer error"));
        emit stateChanged();
        return;
    }
    auto storage = std::move(buffer).value();
    const auto copyBytes = std::min(bytes.size(), storage->size());
    std::memcpy(storage->data(), bytes.data(), copyBytes);
    creator::media::VideoFrame out{};
    out.timestamp = now;
    out.width = frame.width();
    out.height = frame.height();
    out.visibleRect = creator::media::PixelRect{0, 0, frame.width(), frame.height()};
    out.contentWidth = frame.width();
    out.contentHeight = frame.height();
    out.pixelFormat = creator::media::PixelFormat::Bgra8;
    out.platformHandle = std::move(storage);
    fanout_->onVideoFrame(std::move(out));
    ++producedFrames_;
#else
    auto out = frame.toVideoFrame();
    if (!out.hasValue()) {
        publishStatus(tr("Avatar frame handoff error"));
        emit stateChanged();
        return;
    }
    auto outFrame = std::move(out).value();
    outFrame.timestamp = now;
    fanout_->onVideoFrame(std::move(outFrame));
    ++producedFrames_;
#endif
}

}  // namespace creator::app
