#include "app/VideoPreviewItem.h"

#include <QSGNode>

namespace creator::app {

QSGNode* VideoPreviewItem::updatePaintNode(QSGNode* oldNode,
                                            UpdatePaintNodeData*) {
    delete oldNode;
    if (rendererStatus().isEmpty()) {
        postRenderState(false,
                        tr("Native Metal video preview is available on macOS only"));
    }
    return nullptr;
}

}  // namespace creator::app
