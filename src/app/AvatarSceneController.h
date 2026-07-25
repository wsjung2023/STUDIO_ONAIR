#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderPipeline.h"
#include "avatar/CharacterAvatarRenderer.h"
#include "avatar/IAvatarRenderer.h"
#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"
#include "avatar/ITrackingProvider.h"
#include "capture/CaptureFanoutSinks.h"
#include "capture/IVideoFrameSink.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace creator::app {

/// QML-facing owner of the live avatar scene source.
///
/// Wires the real avatar chain together: an ITrackingProvider produces an
/// expression from each tick, AvatarParameterMapper + AvatarRenderPipeline turn
/// it into a rendered BGRA frame, and a VideoFrameFanoutSink delivers that
/// frame to both the preview mailbox and (while a take is live) the recording
/// sink — exactly like DeviceCaptureController does for the camera. The avatar
/// only advances its animation while the webcam is actually capturing, so the
/// webcam's presence drives the avatar.
///
/// The tracking here is SYNTHETIC (see SyntheticFaceTrackingProvider) and the
/// renderer is a placeholder puppet when no Inochi2D runtime/model is present.
/// Both facts are surfaced through trackingLabel/avatarStatus so the UI never
/// implies a real face-driven avatar (CLAUDE.md 9).
class AvatarSceneController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool avatarCapturing READ avatarCapturing NOTIFY stateChanged)
    Q_PROPERTY(bool avatarCanStop READ avatarCanStop NOTIFY stateChanged)
    Q_PROPERTY(QString avatarStatus READ avatarStatus NOTIFY stateChanged)
    Q_PROPERTY(QString trackingLabel READ trackingLabel CONSTANT)
    Q_PROPERTY(quint32 avatarWidth READ avatarWidth CONSTANT)
    Q_PROPERTY(quint32 avatarHeight READ avatarHeight CONSTANT)
    // Selectable characters and live placement. `avatarCharacters` is a list of
    // {key, label} objects for the picker; the others are the current selection.
    Q_PROPERTY(QVariantList avatarCharacters READ avatarCharacters CONSTANT)
    Q_PROPERTY(int avatarCharacterIndex READ avatarCharacterIndex WRITE
                   setAvatarCharacterIndex NOTIFY styleChanged)
    Q_PROPERTY(bool avatarStyleSelectable READ avatarStyleSelectable CONSTANT)
    // True when the avatar can be freely moved/resized -- both the placeholder and
    // a real Inochi2D puppet support this, unlike character selection which is
    // placeholder-only.
    Q_PROPERTY(bool avatarTransformable READ avatarTransformable CONSTANT)
    Q_PROPERTY(int avatarPlacementMode READ avatarPlacementMode WRITE
                   setAvatarPlacementMode NOTIFY styleChanged)
    Q_PROPERTY(int avatarCorner READ avatarCorner WRITE setAvatarCorner NOTIFY
                   styleChanged)
    // Free transform: a size multiplier and a normalised [0,1] head position that
    // a live drag and the size slider drive. These always win over the presets.
    Q_PROPERTY(double avatarScale READ avatarScale WRITE setAvatarScale NOTIFY
                   transformChanged)
    Q_PROPERTY(double avatarPosX READ avatarPosX NOTIFY transformChanged)
    Q_PROPERTY(double avatarPosY READ avatarPosY NOTIFY transformChanged)
    Q_PROPERTY(double avatarMinScale READ avatarMinScale CONSTANT)
    Q_PROPERTY(double avatarMaxScale READ avatarMaxScale CONSTANT)

public:
    /// `cameraLive` reports whether the webcam is currently capturing; the
    /// avatar's expression only advances while it returns true. `usingRealModel`
    /// and `usingRealTracking` record whether a real Inochi2D model / real face
    /// tracker were loaded (both false in the current placeholder build) so the
    /// status text is honest.
    AvatarSceneController(std::unique_ptr<avatar::ITrackingProvider> provider,
                          avatar::AvatarParameterMapper mapper,
                          std::unique_ptr<avatar::IAvatarRenderer> renderer,
                          std::uint32_t width, std::uint32_t height,
                          std::function<bool()> cameraLive,
                          bool usingRealModel, bool usingRealTracking,
                          avatar::CharacterAvatarRenderer* characterControl = nullptr,
                          avatar::inochi2d::Inochi2dAvatarRenderer* inochiControl =
                              nullptr,
                          QObject* parent = nullptr);
    ~AvatarSceneController() override;

    [[nodiscard]] bool avatarCapturing() const noexcept { return capturing_; }
    [[nodiscard]] bool avatarCanStop() const noexcept { return enabled_; }
    [[nodiscard]] QString avatarStatus() const { return status_; }
    [[nodiscard]] QString trackingLabel() const { return trackingLabel_; }
    [[nodiscard]] quint32 avatarWidth() const noexcept { return width_; }
    [[nodiscard]] quint32 avatarHeight() const noexcept { return height_; }
    [[nodiscard]] quint64 producedFrames() const noexcept {
        return producedFrames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] QVariantList avatarCharacters() const;
    [[nodiscard]] bool avatarStyleSelectable() const noexcept {
        return characterControl_ != nullptr;
    }
    [[nodiscard]] bool avatarTransformable() const noexcept {
        return characterControl_ != nullptr || inochiControl_ != nullptr;
    }
    [[nodiscard]] int avatarCharacterIndex() const noexcept { return characterIndex_; }
    void setAvatarCharacterIndex(int index);
    [[nodiscard]] int avatarPlacementMode() const noexcept { return placementMode_; }
    void setAvatarPlacementMode(int mode);
    [[nodiscard]] int avatarCorner() const noexcept { return corner_; }
    void setAvatarCorner(int corner);

    [[nodiscard]] double avatarScale() const noexcept { return scale_; }
    [[nodiscard]] double avatarPosX() const noexcept { return posX_; }
    [[nodiscard]] double avatarPosY() const noexcept { return posY_; }
    [[nodiscard]] double avatarMinScale() const noexcept;
    [[nodiscard]] double avatarMaxScale() const noexcept;

    /// Live size control (size slider). Clamped to the renderer's scale range.
    Q_INVOKABLE void setAvatarScale(double scale);
    /// Live free position (drag). nx, ny are normalised [0,1] frame coordinates.
    Q_INVOKABLE void setAvatarPosition(double nx, double ny);

    /// Renders one avatar frame (at the current animation phase plus
    /// `extraSeconds`) straight to a QImage, using the exact tracking -> map ->
    /// render chain that feeds the preview and recording. Used to prove the
    /// rendered avatar visually without depending on the Qt Quick scene-graph
    /// preview surface. Returns a null image if rendering fails.
    [[nodiscard]] QImage renderDiagnosticImage(double extraSeconds = 0.0);

    [[nodiscard]] std::shared_ptr<creator::capture::LatestVideoFrameMailbox>
    avatarPreviewMailbox() const noexcept {
        return previewMailbox_;
    }
    [[nodiscard]] std::optional<creator::domain::SourceId>
    activeAvatarSourceId() const {
        return capturing_ ? std::optional<creator::domain::SourceId>{sourceId_}
                          : std::nullopt;
    }
    void setAvatarRecordingSink(
        std::shared_ptr<creator::capture::IVideoFrameSink> sink) noexcept;

    Q_INVOKABLE void setAvatarEnabled(bool enabled);

signals:
    void stateChanged();
    void styleChanged();
    /// Emitted when the free transform (size / position) changes, from either a
    /// drag/slider or a placement preset, so the QML size slider stays in sync.
    void transformChanged();
    /// Emitted once per produced frame (~30 fps) so the live preview surface can
    /// repaint. stateChanged fires only on status transitions, which is too rare
    /// to drive a moving avatar, so this is the per-frame repaint trigger.
    void previewFrameReady();

private:
    // One rendered avatar frame (tracking poll -> map -> render -> fanout push).
    // Runs on the dedicated render thread, NOT the UI thread, so a busy UI
    // (recording, MLT, QML) can no longer starve the ~30 fps avatar and freeze it
    // in the recording (CLAUDE.md 9: no heavy work on the UI thread).
    void renderOnce();
    // The render thread's body: paces renderOnce() at ~30 fps until stop.
    void renderLoop(std::stop_token stop);
    void startRenderThread();
    void stopRenderThread() noexcept;
    void publishStatus(QString message);
    // Thread-safe status update: marshals publishStatus + stateChanged onto the
    // owning (UI) thread so the render thread never writes status_ directly.
    void postStatus(QString message);
    void persistStyle();
    void syncTransformFromRenderer();

    std::unique_ptr<avatar::ITrackingProvider> provider_;
    avatar::AvatarParameterMapper mapper_;
    std::unique_ptr<avatar::IAvatarRenderer> renderer_;
    avatar::AvatarRenderPipeline pipeline_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::function<bool()> cameraLive_;
    // Non-owning: aliases renderer_ when it is a CharacterAvatarRenderer, so the
    // picker/placement controls can retune it live. Null when a different
    // renderer (e.g. Inochi2D) is active, and the style controls are disabled.
    avatar::CharacterAvatarRenderer* characterControl_{nullptr};
    // Non-owning: aliases renderer_ when it is an Inochi2dAvatarRenderer, so the
    // size/position controls can retune the real puppet live. Null otherwise.
    avatar::inochi2d::Inochi2dAvatarRenderer* inochiControl_{nullptr};
    int characterIndex_{0};
    int placementMode_{0};
    int corner_{1};
    // Cached free-transform values, mirroring the renderer atomics so the QML
    // properties can read them without touching the renderer off-thread.
    double scale_{1.0};
    double posX_{0.5};
    double posY_{0.46};

    creator::domain::SourceId sourceId_;
    std::shared_ptr<creator::capture::LatestVideoFrameMailbox> previewMailbox_;
    std::shared_ptr<creator::capture::VideoFrameFanoutSink> fanout_;
    std::shared_ptr<creator::capture::IVideoFrameSink> recordingSink_;

    // Serialises the tracking-provider + render chain so the render thread and a
    // UI-thread renderDiagnosticImage() can never poll the provider or rasterise
    // concurrently. The renderer's own live controls (character/placement/scale/
    // position) are already atomic, so the QML mutators do NOT take this lock.
    std::mutex pipelineMutex_;
    // Render-thread pacing/wake. condition_variable_any so the wait can be
    // interrupted by the jthread's stop_token for immediate, clean cancellation.
    std::mutex wakeMutex_;
    std::condition_variable_any wakeCv_;
    std::jthread renderThread_;

    bool enabled_{false};
    bool capturing_{false};
    QString status_;
    QString trackingLabel_;
    creator::core::DurationNs animationPhase_{0};  // guarded by pipelineMutex_
    std::optional<creator::core::TimestampNs> lastTick_;  // guarded by pipelineMutex_
    std::atomic<quint64> producedFrames_{0};
};

}  // namespace creator::app
