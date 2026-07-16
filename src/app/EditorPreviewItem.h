#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>

namespace creator::app {

/// Portable Qt Quick surface for detached CPU preview frames from the edit
/// engine. Frame decoding stays on EditorEngineWorker; paint() only presents
/// the latest immutable QImage on the scene graph's GUI-facing item.
class EditorPreviewItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QImage frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(bool stale READ stale WRITE setStale NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText WRITE setStatusText
                   NOTIFY stateChanged)

public:
    explicit EditorPreviewItem(QQuickItem* parent = nullptr);

    [[nodiscard]] QImage frame() const { return frame_; }
    void setFrame(QImage frame);
    [[nodiscard]] bool hasFrame() const noexcept { return !frame_.isNull(); }

    [[nodiscard]] bool stale() const noexcept { return stale_; }
    void setStale(bool stale);
    [[nodiscard]] QString statusText() const { return statusText_; }
    void setStatusText(QString statusText);

    void paint(QPainter* painter) override;

signals:
    void frameChanged();
    void stateChanged();

private:
    QImage frame_;
    bool stale_{false};
    QString statusText_;
};

}  // namespace creator::app
