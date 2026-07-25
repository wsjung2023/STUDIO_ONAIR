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
#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>

namespace creator::app {

AvatarSceneController::AvatarSceneController(
    std::unique_ptr<avatar::ITrackingProvider> provider,
    avatar::AvatarParameterMapper mapper,
    std::unique_ptr<avatar::IAvatarRenderer> renderer, std::uint32_t width,
    std::uint32_t height, std::function<bool()> cameraLive, bool usingRealModel,
    bool usingRealTracking, avatar::CharacterAvatarRenderer* characterControl,
    avatar::inochi2d::Inochi2dAvatarRenderer* inochiControl, QObject* parent)
    : QObject(parent),
      provider_(std::move(provider)),
      mapper_(std::move(mapper)),
      renderer_(std::move(renderer)),
      pipeline_(mapper_, *renderer_),
      width_(width),
      height_(height),
      cameraLive_(std::move(cameraLive)),
      characterControl_(characterControl),
      inochiControl_(inochiControl),
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
    } else if (inochiControl_ != nullptr) {
        // Real Inochi2D puppet: no character/placement presets, but it supports
        // the same free size/position transform. Restore the creator's persisted
        // size and position so the avatar stays where they put it across restarts.
        scale_ = static_cast<double>(inochiControl_->userScale());
        posX_ = static_cast<double>(inochiControl_->posX());
        posY_ = static_cast<double>(inochiControl_->posY());
        QSettings settings;
        setAvatarScale(
            settings.value(QStringLiteral("avatar/scale"), scale_).toDouble());
        setAvatarPosition(
            settings.value(QStringLiteral("avatar/posX"), posX_).toDouble(),
            settings.value(QStringLiteral("avatar/posY"), posY_).toDouble());
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
}

AvatarSceneController::~AvatarSceneController() { stopRenderThread(); }

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
        {
            std::lock_guard lock{pipelineMutex_};
            animationPhase_ = creator::core::DurationNs{0};
            lastTick_.reset();
        }
        capturing_ = true;
        publishStatus(tr("Avatar live — %1").arg(trackingLabel_));
        // Start the render thread AFTER the fanout/mailbox exist so the worker
        // always sees a valid sink; stop it BEFORE they are reset on disable.
        startRenderThread();
    } else {
        stopRenderThread();
        capturing_ = false;
        fanout_.reset();
        previewMailbox_.reset();
        publishStatus(tr("Avatar stopped"));
    }
    emit stateChanged();
}

void AvatarSceneController::startRenderThread() {
    stopRenderThread();  // ensure no prior worker is running
    renderThread_ = std::jthread([this](std::stop_token stop) { renderLoop(stop); });
}

void AvatarSceneController::stopRenderThread() noexcept {
    if (!renderThread_.joinable()) return;
    renderThread_.request_stop();
    wakeCv_.notify_all();  // wake the pacing wait immediately for a clean join
    renderThread_.join();
}

void AvatarSceneController::renderLoop(std::stop_token stop) {
    // Steady ~30 fps pacing, independent of the UI thread's load. The frame's
    // real timestamp still comes from ProjectClock::now() inside renderOnce().
    constexpr auto kPeriod = std::chrono::milliseconds{33};
    while (!stop.stop_requested()) {
        renderOnce();
        std::unique_lock lock{wakeMutex_};
        // Interruptible wait: returns early when stop is requested, so join is
        // immediate rather than up to one frame late.
        wakeCv_.wait_for(lock, stop, kPeriod, [] { return false; });
    }
}

void AvatarSceneController::postStatus(QString message) {
    // Marshal onto the owning (UI) thread: status_ is read by QML there, so the
    // render thread must never write it directly.
    QMetaObject::invokeMethod(
        this,
        [this, message = std::move(message)]() mutable {
            publishStatus(std::move(message));
            emit stateChanged();
        },
        Qt::QueuedConnection);
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
    if (characterControl_ == nullptr && inochiControl_ != nullptr)
        return static_cast<double>(avatar::inochi2d::Inochi2dAvatarRenderer::kMinScale);
    return static_cast<double>(avatar::CharacterAvatarRenderer::kMinScale);
}

double AvatarSceneController::avatarMaxScale() const noexcept {
    if (characterControl_ == nullptr && inochiControl_ != nullptr)
        return static_cast<double>(avatar::inochi2d::Inochi2dAvatarRenderer::kMaxScale);
    return static_cast<double>(avatar::CharacterAvatarRenderer::kMaxScale);
}

void AvatarSceneController::setAvatarScale(double scale) {
    double applied = scale_;
    if (characterControl_ != nullptr) {
        characterControl_->setUserScale(static_cast<float>(scale));
        applied = static_cast<double>(characterControl_->userScale());
    } else if (inochiControl_ != nullptr) {
        inochiControl_->setUserScale(static_cast<float>(scale));
        applied = static_cast<double>(inochiControl_->userScale());
    } else {
        return;
    }
    if (applied == scale_) return;
    scale_ = applied;
    persistStyle();
    emit transformChanged();
}

void AvatarSceneController::setAvatarPosition(double nx, double ny) {
    double ax = posX_;
    double ay = posY_;
    if (characterControl_ != nullptr) {
        characterControl_->setPosition(static_cast<float>(nx),
                                       static_cast<float>(ny));
        ax = static_cast<double>(characterControl_->posX());
        ay = static_cast<double>(characterControl_->posY());
    } else if (inochiControl_ != nullptr) {
        inochiControl_->setPosition(static_cast<float>(nx), static_cast<float>(ny));
        ax = static_cast<double>(inochiControl_->posX());
        ay = static_cast<double>(inochiControl_->posY());
    } else {
        return;
    }
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
    if (characterControl_ != nullptr) {
        scale_ = static_cast<double>(characterControl_->userScale());
        posX_ = static_cast<double>(characterControl_->posX());
        posY_ = static_cast<double>(characterControl_->posY());
    } else if (inochiControl_ != nullptr) {
        scale_ = static_cast<double>(inochiControl_->userScale());
        posX_ = static_cast<double>(inochiControl_->posX());
        posY_ = static_cast<double>(inochiControl_->posY());
    } else {
        return;
    }
    emit transformChanged();
}

QImage AvatarSceneController::renderDiagnosticImage(double extraSeconds) {
    const auto extra = std::chrono::duration_cast<creator::core::DurationNs>(
        std::chrono::duration<double>(extraSeconds));
    // Serialise against the render thread: both this and renderOnce() poll the
    // same provider and rasterise through the same pipeline.
    std::lock_guard lock{pipelineMutex_};
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

void AvatarSceneController::renderOnce() {
    // fanout_ is created before this thread starts and reset only after it joins,
    // so reading it here is safe; copy it so a push happens off the pipeline lock.
    const auto fanout = fanout_;
    if (!fanout) return;

    const auto now = creator::core::ProjectClock::now();
    // Only gates whether the SYNTHETIC provider's phase advances (the real
    // OpenSeeFace path passes a constant `true`). For synthetic, this reads the
    // device controller's camera state from this thread; that enum read is an
    // aligned, single-instruction load on every target, so at worst the gate is
    // one frame stale when the camera starts/stops — harmless for a "breathe
    // while the camera is on" cue, and never affects the recorded avatar.
    const bool live = cameraLive_ ? cameraLive_() : true;

    creator::media::VideoFrame out{};
    {
        // Serialise the provider poll + rasterise against renderDiagnosticImage()
        // (which polls/renders on the UI thread). The renderer's live controls are
        // atomic and are NOT covered here.
        std::lock_guard lock{pipelineMutex_};

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
            postStatus(tr("Avatar tracking error: %1")
                           .arg(QString::fromStdString(tracked.error().message())));
            return;
        }
        const avatar::AvatarMotionSample sample{
            .timestamp = phaseFrame.timestamp,
            .parameters = tracked.value().raw,
            .provider = provider_->providerId(),
        };
        auto rendered = pipeline_.render(sample);
        if (!rendered.hasValue()) {
            postStatus(tr("Avatar render error: %1")
                           .arg(QString::fromStdString(rendered.error().message())));
            return;
        }
        const auto& frame = rendered.value();
        const auto bytes = frame.bytes();

#if defined(CS_APP_ENABLE_FFMPEG)
        // Wrap the packed BGRA in the CpuBgraFrameBuffer the preview node and the
        // recording encoder both expect (mirrors the camera's Windows frame
        // handle). The copy makes `out` self-contained so it survives the lock.
        auto buffer = creator::ffmpeg_adapter::CpuBgraFrameBuffer::create(
            frame.width(), frame.height());
        if (!buffer.hasValue()) {
            postStatus(tr("Avatar frame buffer error"));
            return;
        }
        auto storage = std::move(buffer).value();
        const auto copyBytes = std::min(bytes.size(), storage->size());
        std::memcpy(storage->data(), bytes.data(), copyBytes);
        out.timestamp = now;
        out.width = frame.width();
        out.height = frame.height();
        out.visibleRect =
            creator::media::PixelRect{0, 0, frame.width(), frame.height()};
        out.contentWidth = frame.width();
        out.contentHeight = frame.height();
        out.pixelFormat = creator::media::PixelFormat::Bgra8;
        out.platformHandle = std::move(storage);
#else
        auto handoff = frame.toVideoFrame();
        if (!handoff.hasValue()) {
            postStatus(tr("Avatar frame handoff error"));
            return;
        }
        out = std::move(handoff).value();
        out.timestamp = now;
#endif
    }  // pipelineMutex_ released before the (thread-safe) fanout push

    fanout->onVideoFrame(std::move(out));
    producedFrames_.fetch_add(1, std::memory_order_relaxed);
    // stateChanged fires too rarely to drive a moving avatar; this per-frame
    // repaint trigger is marshalled onto the UI thread (queued) from here.
    QMetaObject::invokeMethod(
        this, [this] { emit previewFrameReady(); }, Qt::QueuedConnection);
}

}  // namespace creator::app
