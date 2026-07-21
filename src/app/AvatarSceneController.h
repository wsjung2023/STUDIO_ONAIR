#pragma once

#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderPipeline.h"
#include "avatar/CharacterAvatarRenderer.h"
#include "avatar/IAvatarRenderer.h"
#include "avatar/ITrackingProvider.h"
#include "capture/CaptureFanoutSinks.h"
#include "capture/IVideoFrameSink.h"
#include "capture/LatestVideoFrameMailbox.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>
#include <memory>
#include <optional>

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
    Q_PROPERTY(int avatarPlacementMode READ avatarPlacementMode WRITE
                   setAvatarPlacementMode NOTIFY styleChanged)
    Q_PROPERTY(int avatarCorner READ avatarCorner WRITE setAvatarCorner NOTIFY
                   styleChanged)

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
                          QObject* parent = nullptr);
    ~AvatarSceneController() override;

    [[nodiscard]] bool avatarCapturing() const noexcept { return capturing_; }
    [[nodiscard]] bool avatarCanStop() const noexcept { return enabled_; }
    [[nodiscard]] QString avatarStatus() const { return status_; }
    [[nodiscard]] QString trackingLabel() const { return trackingLabel_; }
    [[nodiscard]] quint32 avatarWidth() const noexcept { return width_; }
    [[nodiscard]] quint32 avatarHeight() const noexcept { return height_; }
    [[nodiscard]] quint64 producedFrames() const noexcept { return producedFrames_; }

    [[nodiscard]] QVariantList avatarCharacters() const;
    [[nodiscard]] bool avatarStyleSelectable() const noexcept {
        return characterControl_ != nullptr;
    }
    [[nodiscard]] int avatarCharacterIndex() const noexcept { return characterIndex_; }
    void setAvatarCharacterIndex(int index);
    [[nodiscard]] int avatarPlacementMode() const noexcept { return placementMode_; }
    void setAvatarPlacementMode(int mode);
    [[nodiscard]] int avatarCorner() const noexcept { return corner_; }
    void setAvatarCorner(int corner);

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
    /// Emitted once per produced frame (~30 fps) so the live preview surface can
    /// repaint. stateChanged fires only on status transitions, which is too rare
    /// to drive a moving avatar, so this is the per-frame repaint trigger.
    void previewFrameReady();

private:
    void tick();
    void publishStatus(QString message);

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
    int characterIndex_{0};
    int placementMode_{0};
    int corner_{1};

    creator::domain::SourceId sourceId_;
    std::shared_ptr<creator::capture::LatestVideoFrameMailbox> previewMailbox_;
    std::shared_ptr<creator::capture::VideoFrameFanoutSink> fanout_;
    std::shared_ptr<creator::capture::IVideoFrameSink> recordingSink_;
    QTimer timer_;

    bool enabled_{false};
    bool capturing_{false};
    QString status_;
    QString trackingLabel_;
    creator::core::DurationNs animationPhase_{0};
    std::optional<creator::core::TimestampNs> lastTick_;
    quint64 producedFrames_{0};
};

}  // namespace creator::app
