#include "app/ScreenPreviewItem.h"

#include <QSGNode>

namespace creator::app {

QSGNode* ScreenPreviewItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    delete oldNode;
    if (rendererStatus().isEmpty()) {
        postRenderState(false, tr("Native Metal preview is available on macOS only"));
    }
    return nullptr;
}

}  // namespace creator::app

