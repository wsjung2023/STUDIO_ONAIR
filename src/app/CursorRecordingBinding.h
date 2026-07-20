#pragma once

#include "app/LiveRecordingController.h"
#include "core/Result.h"
#include "cursor/CursorEventPump.h"
#include "cursor/ICursorSource.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <filesystem>
#include <functional>
#include <memory>

namespace creator::app {

/// Owns one optional cursor telemetry stream for the exact lifetime of a live
/// screen recording. Cursor failures are isolated from the media recorder.
class CursorRecordingBinding final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(qulonglong eventCount READ eventCount NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY stateChanged)

public:
    using SourceFactory = std::function<
        core::Result<std::unique_ptr<cursor::ICursorSource>>(
            const LiveRecordingStart&)>;

    CursorRecordingBinding(LiveRecordingController& recording,
                           SourceFactory sourceFactory,
                           QObject* parent = nullptr);
    ~CursorRecordingBinding() override;

    [[nodiscard]] bool available() const noexcept {
        return static_cast<bool>(sourceFactory_);
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] qulonglong eventCount() const noexcept { return eventCount_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QString outputPath() const { return outputPath_; }

public slots:
    void poll();

signals:
    void stateChanged();

private:
    void handleRecordingChanged();
    void begin(const LiveRecordingStart& start);
    void finish();
    void fail(core::AppError error);
    void setStatus(QString status);
    void publishStats();

    QPointer<LiveRecordingController> recording_;
    SourceFactory sourceFactory_;
    QTimer timer_;
    std::unique_ptr<cursor::CursorEventPump> pump_;
    std::filesystem::path partPath_;
    std::filesystem::path finalPath_;
    qulonglong eventCount_{};
    QString statusMessage_;
    QString outputPath_;
    bool active_{};
};

}  // namespace creator::app
