#include "app/ScreenPreviewItem.h"

#include "app/PreviewGeometry.h"
#include "media/MediaTypes.h"

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace creator::app {
namespace {

class MacPreviewNode final : public QSGSimpleTextureNode {
public:
    ~MacPreviewNode() override { clearTexture(); }

    [[nodiscard]] bool replaceFrame(media::VideoFrame frame, QQuickWindow* window) {
        if (window == nullptr ||
            window->rendererInterface()->graphicsApi() != QSGRendererInterface::Metal ||
            frame.pixelFormat != media::PixelFormat::Bgra8 || !frame.platformHandle) {
            return false;
        }
        if (frame.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            frame.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            return false;
        }

        CVPixelBufferRef pixelBuffer =
            static_cast<CVPixelBufferRef>(frame.platformHandle.get());
        IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixelBuffer);
        if (surface == nullptr) return false;

        auto* renderer = window->rendererInterface();
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)renderer->getResource(
                window, QSGRendererInterface::DeviceResource);
        if (device == nil) return false;

        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = MTLTextureType2D;
        descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.width = frame.width;
        descriptor.height = frame.height;
        descriptor.mipmapLevelCount = 1;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> imported = [device newTextureWithDescriptor:descriptor
                                                         iosurface:surface
                                                             plane:0];
        if (imported == nil) return false;

        QSGTexture* wrapper = QNativeInterface::QSGMetalTexture::fromNative(
            imported, window,
            QSize{static_cast<int>(frame.width), static_cast<int>(frame.height)},
            QQuickWindow::TextureHasAlphaChannel);
        if (wrapper == nullptr) return false;

        clearTexture();
        retainedFrame_ = std::move(frame);
        metalTexture_ = imported;
        wrapper_ = wrapper;
        setTexture(wrapper_);
        setOwnsTexture(false);
        setFiltering(QSGTexture::Linear);
        return true;
    }

    [[nodiscard]] QSize frameSize() const noexcept {
        return retainedFrame_
                   ? QSize{static_cast<int>(retainedFrame_->width),
                           static_cast<int>(retainedFrame_->height)}
                   : QSize{};
    }

private:
    void clearTexture() {
        setTexture(nullptr);
        delete wrapper_;
        wrapper_ = nullptr;
        metalTexture_ = nil;
        retainedFrame_.reset();
    }

    std::optional<media::VideoFrame> retainedFrame_;
    id<MTLTexture> __strong metalTexture_{nil};
    QSGTexture* wrapper_{nullptr};
};

}  // namespace

QSGNode* ScreenPreviewItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    auto* node = static_cast<MacPreviewNode*>(oldNode);
    auto frameMailbox = mailbox();
    if (!frameMailbox) {
        delete node;
        postRenderState(false, tr("Start a screen preview to receive frames"));
        return nullptr;
    }
    if (window() == nullptr ||
        window()->rendererInterface()->graphicsApi() != QSGRendererInterface::Metal) {
        delete node;
        postRenderState(false, tr("Qt Quick is not using the Metal renderer"));
        return nullptr;
    }
    if (!node) node = new MacPreviewNode;

    if (auto frame = frameMailbox->takeLatest()) {
        if (!node->replaceFrame(std::move(*frame), window())) {
            delete node;
            postRenderState(false, tr("Zero-copy IOSurface import failed"));
            return nullptr;
        }
        postRenderState(true, tr("Metal zero-copy preview"));
    }

    if (!node->frameSize().isEmpty()) {
        node->setRect(aspectFitRect(node->frameSize(), boundingRect()));
        node->setTextureCoordinatesTransform(QSGSimpleTextureNode::NoTransform);
    } else {
        postRenderState(false, tr("Waiting for the first ScreenCaptureKit frame"));
    }
    return node;
}

}  // namespace creator::app
