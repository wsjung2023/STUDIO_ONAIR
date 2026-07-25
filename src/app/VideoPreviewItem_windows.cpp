#include "app/VideoPreviewItem.h"

#include "app/PreviewGeometry.h"
#include "media/MediaTypes.h"

#if defined(CS_APP_ENABLE_FFMPEG)
#include "ffmpeg_adapter/BgraFrameMappers.h"
#endif

#include <QImage>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace creator::app {
namespace {

/// A tightly-addressable, read-only BGRA view onto one captured frame.
///
/// The neutral media::VideoFrame carries its pixels through a type-erased
/// platformHandle whose concrete payload depends on the capture adapter that
/// produced it. On Windows two adapters feed this preview:
///   * the audited FFmpeg gdigrab/dshow path stores a CpuBgraFrameBuffer, and
///   * the default Windows.Graphics.Capture path stores an aliasing shared_ptr
///     that points straight at the first packed BGRA pixel byte.
/// owner keeps the backing storage alive for as long as the view is read.
struct BgraFrameView final {
    const std::uint8_t* pixels{nullptr};
    std::size_t rowBytes{0};
    std::shared_ptr<void> owner;
};

[[nodiscard]] std::optional<BgraFrameView> bgraViewOf(
    const media::VideoFrame& frame) {
    if (frame.pixelFormat != media::PixelFormat::Bgra8 || !frame.platformHandle ||
        frame.width == 0 || frame.height == 0) {
        return std::nullopt;
    }
#if defined(CS_APP_ENABLE_FFMPEG)
    auto buffer =
        std::static_pointer_cast<ffmpeg_adapter::CpuBgraFrameBuffer>(
            frame.platformHandle);
    if (!buffer || buffer->width() != frame.width ||
        buffer->height() != frame.height ||
        buffer->rowBytes() <
            static_cast<std::size_t>(frame.width) * 4U ||
        buffer->size() < buffer->rowBytes() * frame.height) {
        return std::nullopt;
    }
    return BgraFrameView{.pixels = buffer->data(),
                         .rowBytes = buffer->rowBytes(),
                         .owner = buffer};
#else
    return BgraFrameView{
        .pixels = static_cast<const std::uint8_t*>(frame.platformHandle.get()),
        .rowBytes = static_cast<std::size_t>(frame.width) * 4U,
        .owner = frame.platformHandle};
#endif
}

/// Scene-graph node that owns the RHI texture for exactly the frame currently
/// on screen. It wraps a portable QSGImageNode (works on the Direct3D 11, Vulkan
/// or OpenGL RHI backend Qt Quick picks on Windows), so no graphics API type
/// leaks into the neutral frame path.
class WindowsPreviewNode final : public QSGNode {
public:
    WindowsPreviewNode(QQuickWindow* window, std::uint64_t mailboxGeneration)
        : imageNode_(window->createImageNode()),
          mailboxGeneration_(mailboxGeneration) {
        imageNode_->setOwnsTexture(true);
        imageNode_->setFiltering(QSGTexture::Linear);
        appendChildNode(imageNode_);
    }

    [[nodiscard]] std::uint64_t mailboxGeneration() const noexcept {
        return mailboxGeneration_;
    }
    [[nodiscard]] QSGImageNode* image() const noexcept { return imageNode_; }
    [[nodiscard]] QSize contentSize() const noexcept {
        return geometry_.contentSize;
    }
    [[nodiscard]] QRectF sourceRect() const noexcept {
        return geometry_.sourceRect;
    }

    /// Uploads the frame's BGRA pixels into a fresh texture. Returns false for a
    /// frame the preview cannot represent so the caller can surface a status.
    [[nodiscard]] bool replaceFrame(const media::VideoFrame& frame,
                                    QQuickWindow* window) {
        const auto view = bgraViewOf(frame);
        if (!view || view->pixels == nullptr || window == nullptr) return false;
        if (frame.width >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            frame.height >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return false;
        }

        // Format_ARGB32 stores 0xAARRGGBB, i.e. B,G,R,A in little-endian memory,
        // which matches the packed BGRA capture bytes exactly. These bytes are
        // STRAIGHT (non-premultiplied) alpha, but Qt Quick's scene graph blends
        // textures as PREMULTIPLIED. Opaque sources (screen/camera) are identical
        // either way, but a source with real transparency -- the avatar overlay --
        // blends washed-out and ghostly unless premultiplied first. Convert here so
        // the avatar composites solid. convertToFormat also deep-copies, detaching
        // from the capture buffer before the frame is released.
        const QImage wrapped{view->pixels, static_cast<int>(frame.width),
                             static_cast<int>(frame.height),
                             static_cast<qsizetype>(view->rowBytes),
                             QImage::Format_ARGB32};
        QSGTexture* texture = window->createTextureFromImage(
            wrapped.convertToFormat(QImage::Format_ARGB32_Premultiplied),
            QQuickWindow::TextureHasAlphaChannel);
        if (texture == nullptr) return false;

        imageNode_->setTexture(texture);  // ownsTexture: deletes the previous one
        geometry_ = previewFrameGeometry(frame);
        return true;
    }

private:
    QSGImageNode* imageNode_{nullptr};
    PreviewFrameGeometry geometry_;
    std::uint64_t mailboxGeneration_{0};
};

}  // namespace

QSGNode* VideoPreviewItem::updatePaintNode(QSGNode* oldNode,
                                           UpdatePaintNodeData*) {
    auto* node = static_cast<WindowsPreviewNode*>(oldNode);
    auto snapshot = mailboxSnapshot();
    if (node != nullptr && node->mailboxGeneration() != snapshot.generation) {
        delete node;
        node = nullptr;
    }

    auto frameMailbox = std::move(snapshot.mailbox);
    if (!frameMailbox) {
        delete node;
        postRenderState(false, tr("Start a video source to receive frames"));
        return nullptr;
    }
    if (window() == nullptr) {
        delete node;
        postRenderState(false, tr("Preview window is not ready"));
        return nullptr;
    }

    if (node == nullptr) {
        node = new WindowsPreviewNode{window(), snapshot.generation};
    }

    if (auto frame = frameMailbox->takeLatest()) {
        if (!node->replaceFrame(*frame, window())) {
            delete node;
            postRenderState(false,
                            tr("Preview received an unsupported frame format"));
            return nullptr;
        }
        postRenderState(true, tr("Live screen preview"));
    }

    if (!node->contentSize().isEmpty()) {
        node->image()->setRect(aspectFitRect(node->contentSize(), boundingRect()));
        node->image()->setSourceRect(node->sourceRect());
    } else {
        postRenderState(false, tr("Waiting for the first video frame"));
    }
    return node;
}

}  // namespace creator::app
