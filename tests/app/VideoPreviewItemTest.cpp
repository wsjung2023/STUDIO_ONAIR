#include "app/VideoPreviewItem.h"

#include "capture/LatestVideoFrameMailbox.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>

#include <memory>

namespace {

class PreviewProbe final : public creator::app::VideoPreviewItem {
public:
    explicit PreviewProbe(
        std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox)
    {
        setMailbox(std::move(mailbox));
    }

    void replaceMailbox(
        std::shared_ptr<creator::capture::LatestVideoFrameMailbox> mailbox) {
        setMailbox(std::move(mailbox));
    }
};

void drainQueuedCalls() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
}

TEST(VideoPreviewItemTest, MailboxReplacementPublishesANewRenderGeneration) {
    auto first = std::make_shared<
        creator::capture::LatestVideoFrameMailbox>();
    auto second = std::make_shared<
        creator::capture::LatestVideoFrameMailbox>();
    PreviewProbe preview{first};
    const auto initial = preview.mailboxSnapshot();

    preview.replaceMailbox(first);
    EXPECT_EQ(preview.mailboxSnapshot().generation, initial.generation);
    preview.replaceMailbox(second);
    const auto replaced = preview.mailboxSnapshot();

    EXPECT_GT(replaced.generation, initial.generation);
    EXPECT_EQ(replaced.mailbox, second);
}

TEST(VideoPreviewItemTest, PublishesRenderStateOnlyAfterGuiThreadDispatch) {
    auto mailbox =
        std::make_shared<creator::capture::LatestVideoFrameMailbox>();
    PreviewProbe preview{mailbox};
    QSignalSpy changed{&preview,
                       &creator::app::VideoPreviewItem::renderStateChanged};

    preview.postRenderState(true, QStringLiteral("Native preview ready"));

    EXPECT_FALSE(preview.frameVisible());
    drainQueuedCalls();
    EXPECT_TRUE(preview.frameVisible());
    EXPECT_EQ(preview.rendererStatus(),
              QStringLiteral("Native preview ready"));
    EXPECT_EQ(changed.count(), 1);
    preview.postRenderState(true, QStringLiteral("Native preview ready"));
    drainQueuedCalls();
    EXPECT_EQ(changed.count(), 1);
}

TEST(VideoPreviewItemTest, DestructionReleasesItsRetainedMailboxReference) {
    auto mailbox =
        std::make_shared<creator::capture::LatestVideoFrameMailbox>();
    std::weak_ptr<creator::capture::LatestVideoFrameMailbox> weak{mailbox};
    auto preview = std::make_unique<PreviewProbe>(mailbox);
    mailbox.reset();
    ASSERT_FALSE(weak.expired());

    preview.reset();

    EXPECT_TRUE(weak.expired());
}

TEST(VideoPreviewItemTest, RenderRetentionReleasesPlatformFrameHandle) {
    auto handle = std::make_shared<int>(42);
    std::weak_ptr<void> weak{handle};
    creator::media::VideoFrame frame;
    frame.platformHandle = handle;
    creator::app::VideoPreviewFrameRetention retained;
    retained.replace(std::move(frame));
    handle.reset();
    ASSERT_FALSE(weak.expired());

    retained.reset();

    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(retained.hasFrame());
}

}  // namespace
