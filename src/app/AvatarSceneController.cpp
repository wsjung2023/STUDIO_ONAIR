#include "app/AvatarSceneController.h"

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarRenderFrame.h"
#include "core/AppError.h"

#include <QSettings>
#include <QVariantMap>

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
    bool usingRealTracking, avatar::CharacterAvatarRenderer* characterControl,
    QObject* parent)
    : QObject(parent),
      provider_(std::move(provider)),
      mapper_(std::move(mapper)),
      renderer_(std::move(renderer)),
      pipeline_(mapper_, *renderer_),
      width_(width),
      height_(height),
      cameraLive_(std::move(cameraLive)),
      characterControl_(characterControl),
      sourceId_(creator::domain::SourceId::create("avatar").value()) {
    if (characterControl_ != nullptr) {
        const auto catalog = avatar::avatarCharacterCatalog();
        const auto current = characterControl_->character();
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            if (catalog[i].id == current) {
                characterIndex_ = static_cast<int>(i);
                break;
            }
        }
        const auto placement = characterControl_->placement();
        placementMode_ =
            placement.mode == avatar::AvatarPlacementMode::Corner ? 1 : 0;
        corner_ = static_cast<int>(placement.corner);
        scale_ = characterControl_->userScale();
        posX_ = characterControl_->posX();
        posY_ = characterControl_->posY();
        // Restore the creator's persisted avatar style so character, position
        // and size survive app restarts. Without this every launch snapped the
        // avatar back to the centred, large default and recordings no longer
        // matched where the creator had placed it. Placement/corner are applied
        // BEFORE scale/position because the presets snap the free transform.
        QSettings settings;
        if (settings.contains(QStringLiteral("avatar/characterIndex"))) {
            setAvatarCharacterIndex(
                settings.value(QStringLiteral("avatar/characterIndex")).toInt());
            setAvatarPlacementMode(
                settings.value(QStringLiteral("avatar/placementMode"),
                               placementMode_).toInt());
            setAvatarCorner(
                settings.value(QStringLiteral("avatar/corner"), corner_).toInt());
            setAvatarScale(
                settings.value(QStringLiteral("avatar/scale"), scale_).toDouble());
            setAvatarPosition(
                settings.value(QStringLiteral("avatar/posX"), posX_).toDouble(),
                settings.value(QStringLiteral("avatar/posY"), posY_).toDouble());
        } else {
            // First run: snap to a small bottom-right corner overlay. The
            // renderer already starts in Corner MODE, so setAvatarPlacementMode
            // would early-return without ever applying the corner PRESET that
            // moves/sizes the avatar -- which is why it appeared centred and
            // large. Apply the preset directly.
            placementMode_ = 1;
            corner_ = 1;
            characterControl_->setPlacementMode(avatar::AvatarPlacementMode::Corner);
            characterControl_->setPlacementCorner(avatar::AvatarCorner::RightBottom);
            characterControl_->applyCornerPreset(avatar::AvatarCorner::RightBottom);
            syncTransformFromRenderer();
            persistStyle();
        }
    }
    const QString trackingKind = usingRealTracking
                                     ? tr("live face tracking")
                                     : tr("synthetic (fake) tracking");
    const QString modelKind =
        usingRealModel ? tr("Inochi2D model")
                       : (characterControl_ != nullptr ? tr("built-in character")
                                                       : tr("placeholder avatar"));
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

QVariantList AvatarSceneController::avatarCharacters() const {
    QVariantList list;
    for (const auto& info : avatar::avatarCharacterCatalog()) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), QString::fromStdString(info.key));
        m.insert(QStringLiteral("label"), QString::fromStdString(info.labelKo));
        list.append(m);
    }
    return list;
}

void AvatarSceneController::setAvatarCharacterIndex(int index) {
    const auto catalog = avatar::avatarCharacterCatalog();
    if (index < 0 || index >= static_cast<int>(catalog.size())) return;
    if (index == characterIndex_) return;
    characterIndex_ = index;
    if (characterControl_ != nullptr) {
        characterControl_->setCharacter(catalog[static_cast<std::size_t>(index)].id);
    }
    persistStyle();
    emit styleChanged();
}

void AvatarSceneController::setAvatarPlacementMode(int mode) {
    const int clamped = mode == 1 ? 1 : 0;
    if (clamped == placementMode_) return;
    placementMode_ = clamped;
    if (characterControl_ != nullptr) {
        // Switching style also snaps the free transform to that style's preset
        // (front = centred + large + opaque; corner = the current corner + small).
        if (clamped == 1) {
            characterControl_->setPlacementMode(avatar::AvatarPlacementMode::Corner);
            characterControl_->applyCornerPreset(
                static_cast<avatar::AvatarCorner>(corner_));
        } else {
            characterControl_->setPlacementMode(avatar::AvatarPlacementMode::Front);
            characterControl_->applyFrontPreset();
        }
        syncTransformFromRenderer();
    }
    persistStyle();
    emit styleChanged();
}

void AvatarSceneController::setAvatarCorner(int corner) {
    if (corner < 0 || corner > 3 || corner == corner_) return;
    corner_ = corner;
    if (characterControl_ != nullptr) {
        characterControl_->setPlacementCorner(
            static_cast<avatar::AvatarCorner>(corner));
        // Picking a corner also moves/sizes the avatar to that corner preset.
        characterControl_->applyCornerPreset(
            static_cast<avatar::AvatarCorner>(corner));
        syncTransformFromRenderer();
    }
    persistStyle();
    emit styleChanged();
}

double AvatarSceneController::avatarMinScale() const noexcept {
    return static_cast<double>(avatar::CharacterAvatarRenderer::kMinScale);
}

double AvatarSceneController::avatarMaxScale() const noexcept {
    return static_cast<double>(avatar::CharacterAvatarRenderer::kMaxScale);
}

void AvatarSceneController::setAvatarScale(double scale) {
    if (characterControl_ == nullptr) return;
    characterControl_->setUserScale(static_cast<float>(scale));
    const double applied = static_cast<double>(characterControl_->userScale());
    if (applied == scale_) return;
    scale_ = applied;
    persistStyle();
    emit transformChanged();
}

void AvatarSceneController::setAvatarPosition(double nx, double ny) {
    if (characterControl_ == nullptr) return;
    characterControl_->setPosition(static_cast<float>(nx),
                                   static_cast<float>(ny));
    const double ax = static_cast<double>(characterControl_->posX());
    const double ay = static_cast<double>(characterControl_->posY());
    if (ax == posX_ && ay == posY_) return;
    posX_ = ax;
    posY_ = ay;
    persistStyle();
    emit transformChanged();
}

void AvatarSceneController::persistStyle() {
    QSettings settings;
    settings.setValue(QStringLiteral("avatar/characterIndex"), characterIndex_);
    settings.setValue(QStringLiteral("avatar/placementMode"), placementMode_);
    settings.setValue(QStringLiteral("avatar/corner"), corner_);
    settings.setValue(QStringLiteral("avatar/scale"), scale_);
    settings.setValue(QStringLiteral("avatar/posX"), posX_);
    settings.setValue(QStringLiteral("avatar/posY"), posY_);
}

void AvatarSceneController::syncTransformFromRenderer() {
    if (characterControl_ == nullptr) return;
    scale_ = static_cast<double>(characterControl_->userScale());
    posX_ = static_cast<double>(characterControl_->posX());
    posY_ = static_cast<double>(characterControl_->posY());
    emit transformChanged();
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
    emit previewFrameReady();
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
    emit previewFrameReady();
#endif
}

}  // namespace creator::app
