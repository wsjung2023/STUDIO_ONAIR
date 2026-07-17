#include "app/ShortcutSettingsController.h"

#include <QDir>
#include <QKeyCombination>
#include <QKeySequence>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QVariantMap>

#include <array>
#include <optional>
#include <ranges>
#include <utility>

namespace creator::app {
namespace {

constexpr auto kSettingsGroup = "StudioShortcuts/v1";

const std::array<QString, 13>& actionIds() {
    static const std::array<QString, 13> ids{
        QStringLiteral("record"),        QStringLiteral("marker"),
        QStringLiteral("previousScene"), QStringLiteral("nextScene"),
        QStringLiteral("scene1"),        QStringLiteral("scene2"),
        QStringLiteral("scene3"),        QStringLiteral("scene4"),
        QStringLiteral("scene5"),        QStringLiteral("scene6"),
        QStringLiteral("scene7"),        QStringLiteral("scene8"),
        QStringLiteral("scene9")};
    return ids;
}

QHash<QString, QString> defaultShortcuts() {
    return {{QStringLiteral("record"), QStringLiteral("Ctrl+Shift+R")},
            {QStringLiteral("marker"), QStringLiteral("M")},
            {QStringLiteral("previousScene"), QStringLiteral("Ctrl+PgUp")},
            {QStringLiteral("nextScene"), QStringLiteral("Ctrl+PgDown")},
            {QStringLiteral("scene1"), QStringLiteral("Ctrl+1")},
            {QStringLiteral("scene2"), QStringLiteral("Ctrl+2")},
            {QStringLiteral("scene3"), QStringLiteral("Ctrl+3")},
            {QStringLiteral("scene4"), QStringLiteral("Ctrl+4")},
            {QStringLiteral("scene5"), QStringLiteral("Ctrl+5")},
            {QStringLiteral("scene6"), QStringLiteral("Ctrl+6")},
            {QStringLiteral("scene7"), QStringLiteral("Ctrl+7")},
            {QStringLiteral("scene8"), QStringLiteral("Ctrl+8")},
            {QStringLiteral("scene9"), QStringLiteral("Ctrl+9")}};
}

QString defaultSettingsPath() {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir{directory}.filePath(QStringLiteral("shortcuts.ini"));
}

QString fromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
#endif
}

std::filesystem::path toPath(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    const QByteArray utf8 = path.toUtf8();
    return std::filesystem::path{utf8.constData()};
#endif
}

std::optional<QString> canonicalSequence(const QString& text) {
    if (text.trimmed().isEmpty()) return std::nullopt;
    const QKeySequence sequence =
        QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
    if (sequence.isEmpty()) return std::nullopt;
    for (int index = 0; index < sequence.count(); ++index) {
        if (sequence[index].key() == Qt::Key_unknown) return std::nullopt;
    }
    const QString canonical = sequence.toString(QKeySequence::PortableText);
    if (canonical.isEmpty()) return std::nullopt;
    return canonical;
}

bool isReserved(const QString& canonical) {
    static const std::array<QString, 6> reserved{
        QStringLiteral("Ctrl+Q"), QStringLiteral("Ctrl+S"),
        QStringLiteral("Ctrl+Z"), QStringLiteral("Ctrl+Shift+Z"),
        QStringLiteral("Ctrl+O"), QStringLiteral("Ctrl+N")};
    return std::ranges::find(reserved, canonical) != reserved.end();
}

class ShortcutSettingsWorker final : public QObject {
    Q_OBJECT

public slots:
    void load(const QString& path, quint64 generation) {
        QSettings settings{path, QSettings::IniFormat};
        QVariantMap values;
        settings.beginGroup(QString::fromLatin1(kSettingsGroup));
        for (const QString& actionId : actionIds()) {
            if (settings.contains(actionId)) {
                values.insert(actionId, settings.value(actionId));
            }
        }
        settings.endGroup();
        settings.sync();
        const bool success = settings.status() == QSettings::NoError;
        emit loaded(generation, success, values,
                    success ? QString{}
                            : QStringLiteral("Unable to load shortcut settings."));
    }

    void store(const QString& path, const QString& actionId,
               const QString& sequence, quint64 generation) {
        QSettings settings{path, QSettings::IniFormat};
        settings.beginGroup(QString::fromLatin1(kSettingsGroup));
        settings.setValue(actionId, sequence);
        settings.endGroup();
        settings.sync();
        const bool success = settings.status() == QSettings::NoError;
        emit stored(generation, success, actionId, sequence,
                    success ? QString{}
                            : QStringLiteral("Unable to save shortcut settings."));
    }

signals:
    void loaded(quint64 generation, bool success, QVariantMap values,
                QString error);
    void stored(quint64 generation, bool success, QString actionId,
                QString sequence, QString error);
};

}  // namespace

ShortcutSettingsController::ShortcutSettingsController(QObject* parent)
    : ShortcutSettingsController(toPath(defaultSettingsPath()), parent) {}

ShortcutSettingsController::ShortcutSettingsController(
    std::filesystem::path settingsPath, QObject* parent)
    : QObject(parent),
      settingsPath_(fromPath(settingsPath)),
      shortcuts_(defaultShortcuts()),
      worker_(new ShortcutSettingsWorker) {
    auto* worker = static_cast<ShortcutSettingsWorker*>(worker_);
    worker->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &ShortcutSettingsController::settingsLoadRequested, worker,
            &ShortcutSettingsWorker::load, Qt::QueuedConnection);
    connect(this, &ShortcutSettingsController::settingsStoreRequested, worker,
            &ShortcutSettingsWorker::store, Qt::QueuedConnection);
    connect(worker, &ShortcutSettingsWorker::loaded, this,
            &ShortcutSettingsController::finishLoad, Qt::QueuedConnection);
    connect(worker, &ShortcutSettingsWorker::stored, this,
            &ShortcutSettingsController::finishStore, Qt::QueuedConnection);
    workerThread_.start();
    setBusy(true);
    emit settingsLoadRequested(settingsPath_, ++generation_);
}

ShortcutSettingsController::~ShortcutSettingsController() {
    ++generation_;
    disconnect(worker_, nullptr, this, nullptr);
    disconnect(this, nullptr, worker_, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

QString ShortcutSettingsController::recordShortcut() const {
    return value(QStringLiteral("record"));
}
QString ShortcutSettingsController::markerShortcut() const {
    return value(QStringLiteral("marker"));
}
QString ShortcutSettingsController::previousSceneShortcut() const {
    return value(QStringLiteral("previousScene"));
}
QString ShortcutSettingsController::nextSceneShortcut() const {
    return value(QStringLiteral("nextScene"));
}
QString ShortcutSettingsController::scene1Shortcut() const {
    return value(QStringLiteral("scene1"));
}
QString ShortcutSettingsController::scene2Shortcut() const {
    return value(QStringLiteral("scene2"));
}
QString ShortcutSettingsController::scene3Shortcut() const {
    return value(QStringLiteral("scene3"));
}
QString ShortcutSettingsController::scene4Shortcut() const {
    return value(QStringLiteral("scene4"));
}
QString ShortcutSettingsController::scene5Shortcut() const {
    return value(QStringLiteral("scene5"));
}
QString ShortcutSettingsController::scene6Shortcut() const {
    return value(QStringLiteral("scene6"));
}
QString ShortcutSettingsController::scene7Shortcut() const {
    return value(QStringLiteral("scene7"));
}
QString ShortcutSettingsController::scene8Shortcut() const {
    return value(QStringLiteral("scene8"));
}
QString ShortcutSettingsController::scene9Shortcut() const {
    return value(QStringLiteral("scene9"));
}

void ShortcutSettingsController::setShortcut(QString actionId,
                                             QString sequence) {
    if (busy_) {
        setStatusMessage(QStringLiteral("Shortcut settings are busy."));
        return;
    }
    if (!shortcuts_.contains(actionId)) {
        setStatusMessage(QStringLiteral("Unknown shortcut action."));
        return;
    }
    const auto canonical = canonicalSequence(sequence);
    if (!canonical.has_value()) {
        setStatusMessage(QStringLiteral("Shortcut is invalid."));
        return;
    }
    if (isReserved(*canonical)) {
        setStatusMessage(
            QStringLiteral("Shortcut is reserved by the application."));
        return;
    }
    for (auto iterator = shortcuts_.cbegin(); iterator != shortcuts_.cend();
         ++iterator) {
        if (iterator.key() != actionId && iterator.value() == *canonical) {
            setStatusMessage(QStringLiteral("Shortcut is already in use."));
            return;
        }
    }
    if (shortcuts_.value(actionId) == *canonical) {
        setStatusMessage({});
        return;
    }
    setStatusMessage({});
    setBusy(true);
    emit settingsStoreRequested(settingsPath_, std::move(actionId), *canonical,
                                ++generation_);
}

void ShortcutSettingsController::finishLoad(quint64 generation, bool success,
                                            const QVariantMap& stored,
                                            const QString& error) {
    if (generation != generation_) return;
    if (success) {
        auto candidate = shortcuts_;
        for (const QString& actionId : actionIds()) {
            const auto found = stored.constFind(actionId);
            if (found == stored.cend()) continue;
            const auto canonical = canonicalSequence(found->toString());
            if (!canonical.has_value() || isReserved(*canonical)) continue;
            candidate.insert(actionId, *canonical);
        }
        QSet<QString> uniqueSequences;
        for (const QString& actionId : actionIds()) {
            uniqueSequences.insert(candidate.value(actionId));
        }
        const bool hasConflict =
            uniqueSequences.size() != static_cast<qsizetype>(actionIds().size());
        auto loaded = hasConflict ? defaultShortcuts() : std::move(candidate);
        if (loaded != shortcuts_) {
            shortcuts_ = std::move(loaded);
            emit shortcutsChanged();
        }
        setStatusMessage({});
    } else {
        setStatusMessage(error);
    }
    setBusy(false);
}

void ShortcutSettingsController::finishStore(quint64 generation, bool success,
                                             const QString& actionId,
                                             const QString& sequence,
                                             const QString& error) {
    if (generation != generation_) return;
    if (success) {
        shortcuts_.insert(actionId, sequence);
        setStatusMessage({});
        emit shortcutsChanged();
    } else {
        setStatusMessage(error);
    }
    setBusy(false);
}

void ShortcutSettingsController::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void ShortcutSettingsController::setStatusMessage(QString message) {
    if (statusMessage_ == message) return;
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

QString ShortcutSettingsController::value(const QString& actionId) const {
    return shortcuts_.value(actionId);
}

}  // namespace creator::app

#include "ShortcutSettingsController.moc"
