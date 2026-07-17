#include "app/VideoPreviewItem.h"

#include <QMetaObject>
#include <QPointer>

#include <utility>

namespace creator::app {

void VideoPreviewFrameRetention::replace(media::VideoFrame frame) {
    frame_ = std::move(frame);
}

void VideoPreviewFrameRetention::reset() noexcept {
    frame_.reset();
}

VideoPreviewItem::VideoPreviewItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

std::shared_ptr<capture::LatestVideoFrameMailbox>
VideoPreviewItem::mailbox() const noexcept {
    std::scoped_lock lock{mailboxMutex_};
    return mailbox_;
}

VideoPreviewItem::MailboxSnapshot
VideoPreviewItem::mailboxSnapshot() const noexcept {
    std::scoped_lock lock{mailboxMutex_};
    return {.mailbox = mailbox_, .generation = mailboxGeneration_};
}

void VideoPreviewItem::setMailbox(
    std::shared_ptr<capture::LatestVideoFrameMailbox> mailbox) {
    std::scoped_lock lock{mailboxMutex_};
    if (mailbox_ == mailbox) return;
    mailbox_ = std::move(mailbox);
    ++mailboxGeneration_;
}

void VideoPreviewItem::postRenderState(bool frameVisible, QString status) {
    QPointer<VideoPreviewItem> self{this};
    QMetaObject::invokeMethod(
        this,
        [self, frameVisible, status = std::move(status)]() mutable {
            if (self) self->applyRenderState(frameVisible, std::move(status));
        },
        Qt::QueuedConnection);
}

void VideoPreviewItem::applyRenderState(bool frameVisible, QString status) {
    if (frameVisible_ == frameVisible && rendererStatus_ == status) return;
    frameVisible_ = frameVisible;
    rendererStatus_ = std::move(status);
    emit renderStateChanged();
}

}  // namespace creator::app
