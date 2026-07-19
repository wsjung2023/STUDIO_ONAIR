#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <QThread>
#include <QVariantMap>

#include <filesystem>

namespace creator::app {

class ShortcutSettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString recordShortcut READ recordShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString markerShortcut READ markerShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString previousSceneShortcut READ previousSceneShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString nextSceneShortcut READ nextSceneShortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene1Shortcut READ scene1Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene2Shortcut READ scene2Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene3Shortcut READ scene3Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene4Shortcut READ scene4Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene5Shortcut READ scene5Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene6Shortcut READ scene6Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene7Shortcut READ scene7Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene8Shortcut READ scene8Shortcut NOTIFY shortcutsChanged)
    Q_PROPERTY(QString scene9Shortcut READ scene9Shortcut NOTIFY shortcutsChanged)

public:
    explicit ShortcutSettingsController(QObject* parent = nullptr);
    explicit ShortcutSettingsController(std::filesystem::path settingsPath,
                                        QObject* parent = nullptr);
    ~ShortcutSettingsController() override;

    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }
    [[nodiscard]] QString recordShortcut() const;
    [[nodiscard]] QString markerShortcut() const;
    [[nodiscard]] QString previousSceneShortcut() const;
    [[nodiscard]] QString nextSceneShortcut() const;
    [[nodiscard]] QString scene1Shortcut() const;
    [[nodiscard]] QString scene2Shortcut() const;
    [[nodiscard]] QString scene3Shortcut() const;
    [[nodiscard]] QString scene4Shortcut() const;
    [[nodiscard]] QString scene5Shortcut() const;
    [[nodiscard]] QString scene6Shortcut() const;
    [[nodiscard]] QString scene7Shortcut() const;
    [[nodiscard]] QString scene8Shortcut() const;
    [[nodiscard]] QString scene9Shortcut() const;

    Q_INVOKABLE void setShortcut(QString actionId, QString sequence);

signals:
    void busyChanged();
    void statusMessageChanged();
    void shortcutsChanged();

    void settingsLoadRequested(QString path, quint64 generation);
    void settingsStoreRequested(QString path, QString actionId,
                                QString sequence, quint64 generation);

private:
    void finishLoad(quint64 generation, bool success,
                    const QVariantMap& stored, const QString& error);
    void finishStore(quint64 generation, bool success,
                     const QString& actionId, const QString& sequence,
                     const QString& error);
    void setBusy(bool busy);
    void setStatusMessage(QString message);
    [[nodiscard]] QString value(const QString& actionId) const;

    QString settingsPath_;
    QHash<QString, QString> shortcuts_;
    QObject* worker_{nullptr};
    QThread workerThread_;
    quint64 generation_{0};
    bool busy_{false};
    QString statusMessage_;
};

}  // namespace creator::app
