#pragma once

#include "capture/LatestVideoFrameMailbox.h"

#include <QQuickItem>
#include <QString>

#include <memory>
#include <cstdint>
#include <mutex>
#include <optional>

namespace creator::app {

/// Owns the platform handle for exactly the frame displayed by a render node.
class VideoPreviewFrameRetention final {
public:
    void replace(creator::media::VideoFrame frame);
    void reset() noexcept;
    [[nodiscard]] bool hasFrame() const noexcept { return frame_.has_value(); }

private:
    std::optional<creator::media::VideoFrame> frame_;
};

/// Shared Qt Quick render surface for bounded native video-frame mailboxes.
class VideoPreviewItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool frameVisible READ frameVisible NOTIFY renderStateChanged)
    Q_PROPERTY(QString rendererStatus READ rendererStatus NOTIFY renderStateChanged)

public:
    struct MailboxSnapshot final {
        std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox;
        std::uint64_t generation{0};
    };

    explicit VideoPreviewItem(QQuickItem* parent = nullptr);
    ~VideoPreviewItem() override = default;

    [[nodiscard]] bool frameVisible() const noexcept { return frameVisible_; }
    [[nodiscard]] QString rendererStatus() const { return rendererStatus_; }
    [[nodiscard]] std::shared_ptr<
        creator::capture::LatestVideoFrameMailbox>
    mailbox() const noexcept;
    [[nodiscard]] MailboxSnapshot mailboxSnapshot() const noexcept;

    /// Safe render-thread to GUI-thread state publication.
    void postRenderState(bool frameVisible, QString status);

signals:
    void renderStateChanged();

protected:
    void setMailbox(
        std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox);
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;

private:
    void applyRenderState(bool frameVisible, QString status);

    bool frameVisible_{false};
    QString rendererStatus_;
    mutable std::mutex mailboxMutex_;
    std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox_;
    std::uint64_t mailboxGeneration_{0};
};

}  // namespace creator::app
