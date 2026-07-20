#include "app/CommercialControlsController.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <array>
#include <filesystem>
#include <system_error>
#include <utility>

namespace creator::app {
namespace {

namespace fs = std::filesystem;
enum class Operation { Load = 0, StoreConsent = 1, SignOut = 2, DeleteData = 3 };

fs::path toPath(const QString& path) {
#ifdef _WIN32
    return fs::path{path.toStdWString()};
#else
    const auto utf8 = path.toUtf8();
    return fs::path{utf8.constData()};
#endif
}

QString fromPath(const fs::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
#endif
}

fs::path defaultApplicationStateRoot() {
    return toPath(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation));
}

QString stateName(platform_release::EntitlementState state) {
    switch (state) {
        case platform_release::EntitlementState::Active:
            return QStringLiteral("active");
        case platform_release::EntitlementState::OfflineGrace:
            return QStringLiteral("offline-grace");
        case platform_release::EntitlementState::Unavailable:
            return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

bool isStrictDescendant(const fs::path& child, const fs::path& root) {
    auto childPart = child.begin();
    for (auto rootPart = root.begin(); rootPart != root.end();
         ++rootPart, ++childPart) {
        if (childPart == child.end() || *childPart != *rootPart) return false;
    }
    return childPart != child.end();
}

bool isLink(const fs::path& path, std::error_code& error) {
    const auto status = fs::symlink_status(path, error);
    return !error && fs::is_symlink(status);
}

bool validatedRoots(const QString& applicationRootText,
                    const QString& accountRootText, fs::path& applicationRoot,
                    fs::path& accountRoot, QString& errorMessage) {
    std::error_code error;
    applicationRoot = fs::absolute(toPath(applicationRootText), error).lexically_normal();
    if (error) {
        errorMessage = QStringLiteral("Application state root is invalid.");
        return false;
    }
    accountRoot = fs::absolute(toPath(accountRootText), error).lexically_normal();
    if (error || applicationRoot.empty() || accountRoot.empty() ||
        applicationRoot == applicationRoot.root_path() ||
        !isStrictDescendant(accountRoot, applicationRoot) ||
        accountRoot.filename().empty()) {
        errorMessage = QStringLiteral("Account state root is not a narrow application path.");
        return false;
    }
    if (fs::exists(applicationRoot, error)) {
        if (error || isLink(applicationRoot, error) || error) {
            errorMessage = QStringLiteral("Application state root must not be linked.");
            return false;
        }
    } else if (error) {
        errorMessage = QStringLiteral("Application state root is unavailable.");
        return false;
    }
    if (fs::exists(accountRoot, error)) {
        if (error || isLink(accountRoot, error) || error) {
            errorMessage = QStringLiteral("Account state root must not be linked.");
            return false;
        }
    } else if (error) {
        errorMessage = QStringLiteral("Account state root is unavailable.");
        return false;
    }
    return true;
}

bool readConsent(const fs::path& accountRoot, bool& consent,
                 QString& errorMessage) {
    const auto path = accountRoot / "privacy.json";
    std::error_code error;
    if (!fs::exists(path, error)) {
        if (error) {
            errorMessage = QStringLiteral("Privacy settings are unavailable.");
            return false;
        }
        consent = false;
        return true;
    }
    if (isLink(path, error) || error || !fs::is_regular_file(path, error) || error ||
        fs::file_size(path, error) > 4096 || error) {
        errorMessage = QStringLiteral("Privacy settings are invalid.");
        return false;
    }
    QFile input{fromPath(path)};
    if (!input.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("Privacy settings could not be read.");
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        errorMessage = QStringLiteral("Privacy settings are malformed.");
        return false;
    }
    const auto object = document.object();
    auto keys = object.keys();
    keys.sort();
    if (keys != QStringList{QStringLiteral("diagnosticsConsent"),
                            QStringLiteral("schemaVersion")} ||
        object.value(QStringLiteral("schemaVersion")).toInt(-1) != 1 ||
        !object.value(QStringLiteral("diagnosticsConsent")).isBool()) {
        errorMessage = QStringLiteral("Privacy settings contain unknown fields.");
        return false;
    }
    consent = object.value(QStringLiteral("diagnosticsConsent")).toBool();
    return true;
}

bool writeConsent(const fs::path& applicationRoot, const fs::path& accountRoot,
                  bool consent, QString& errorMessage) {
    std::error_code error;
    fs::create_directories(applicationRoot, error);
    if (!error) fs::create_directories(accountRoot, error);
    if (error) {
        errorMessage = QStringLiteral("Privacy settings directory could not be created.");
        return false;
    }
    const auto privacyPath = accountRoot / "privacy.json";
    if (isLink(accountRoot, error) || error) {
        errorMessage = QStringLiteral("Account state root must not be linked.");
        return false;
    }
    if (fs::exists(privacyPath, error) &&
        (isLink(privacyPath, error) || error ||
         !fs::is_regular_file(privacyPath, error) || error)) {
        errorMessage = QStringLiteral("Privacy settings must not be linked.");
        return false;
    }
    if (error) {
        errorMessage = QStringLiteral("Privacy settings path is unavailable.");
        return false;
    }
    QSaveFile output{fromPath(privacyPath)};
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        errorMessage = QStringLiteral("Privacy settings could not be opened.");
        return false;
    }
    const QJsonObject object{{QStringLiteral("diagnosticsConsent"), consent},
                             {QStringLiteral("schemaVersion"), 1}};
    const auto encoded = QJsonDocument{object}.toJson(QJsonDocument::Compact) + '\n';
    if (output.write(encoded) != encoded.size() || !output.commit()) {
        errorMessage = QStringLiteral("Privacy settings could not be atomically saved.");
        return false;
    }
    return true;
}

bool removeLocalSession(const fs::path& accountRoot, QString& errorMessage) {
    std::error_code error;
    if (!fs::exists(accountRoot, error)) return !error;
    for (const auto* name : {"session.json", "entitlement-state.json"}) {
        const auto path = accountRoot / name;
        if (!fs::exists(path, error)) {
            if (error) break;
            continue;
        }
        if (isLink(path, error) || error || !fs::is_regular_file(path, error) ||
            error || !fs::remove(path, error) || error) {
            errorMessage = QStringLiteral("Local account session could not be removed.");
            return false;
        }
    }
    if (error) {
        errorMessage = QStringLiteral("Local account session is unavailable.");
        return false;
    }
    return true;
}

bool removeAccountRoot(const fs::path& accountRoot, QString& errorMessage) {
    std::error_code error;
    if (!fs::exists(accountRoot, error)) return !error;
    fs::recursive_directory_iterator iterator{accountRoot, error};
    const fs::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (fs::is_symlink(iterator->symlink_status(error))) break;
        iterator.increment(error);
    }
    if (error || iterator != end) {
        errorMessage = QStringLiteral("Linked account data cannot be deleted automatically.");
        return false;
    }
    fs::remove_all(accountRoot, error);
    if (error) {
        errorMessage = QStringLiteral("Local account data could not be deleted.");
        return false;
    }
    return true;
}

class CommercialControlsWorker final : public QObject {
    Q_OBJECT

public slots:
    void load(const QString& applicationRootText, const QString& accountRootText,
              quint64 generation) {
        fs::path applicationRoot;
        fs::path accountRoot;
        QString error;
        bool consent = false;
        const bool valid = validatedRoots(applicationRootText, accountRootText,
                                          applicationRoot, accountRoot, error);
        const bool success = valid && readConsent(accountRoot, consent, error);
        emit finished(generation, static_cast<int>(Operation::Load), success,
                      consent, error);
    }

    void storeConsent(const QString& applicationRootText,
                      const QString& accountRootText, bool consent,
                      quint64 generation) {
        fs::path applicationRoot;
        fs::path accountRoot;
        QString error;
        const bool valid = validatedRoots(applicationRootText, accountRootText,
                                          applicationRoot, accountRoot, error);
        const bool success = valid &&
                             writeConsent(applicationRoot, accountRoot, consent, error);
        emit finished(generation, static_cast<int>(Operation::StoreConsent),
                      success, consent, error);
    }

    void signOut(const QString& applicationRootText,
                 const QString& accountRootText, quint64 generation) {
        fs::path applicationRoot;
        fs::path accountRoot;
        QString error;
        const bool valid = validatedRoots(applicationRootText, accountRootText,
                                          applicationRoot, accountRoot, error);
        const bool success = valid && removeLocalSession(accountRoot, error);
        emit finished(generation, static_cast<int>(Operation::SignOut), success,
                      false, error);
    }

    void deleteData(const QString& applicationRootText,
                    const QString& accountRootText, quint64 generation) {
        fs::path applicationRoot;
        fs::path accountRoot;
        QString error;
        const bool valid = validatedRoots(applicationRootText, accountRootText,
                                          applicationRoot, accountRoot, error);
        const bool success = valid && removeAccountRoot(accountRoot, error);
        emit finished(generation, static_cast<int>(Operation::DeleteData), success,
                      false, error);
    }

signals:
    void finished(quint64 generation, int operation, bool success, bool consent,
                  QString error);
};

}  // namespace

CommercialControlsController::CommercialControlsController(
    platform_release::EntitlementDecision decision, QObject* parent)
    : CommercialControlsController(defaultApplicationStateRoot(),
                                   defaultApplicationStateRoot() / "account-state",
                                   std::move(decision), parent) {}

CommercialControlsController::CommercialControlsController(
    fs::path applicationStateRoot, fs::path accountStateRoot,
    platform_release::EntitlementDecision decision, QObject* parent)
    : QObject(parent),
      applicationStateRoot_(fromPath(applicationStateRoot)),
      accountStateRoot_(fromPath(accountStateRoot)),
      entitlementState_(stateName(decision.state)),
      entitlementReason_(QString::fromStdString(decision.reason)),
      worker_(new CommercialControlsWorker) {
    auto* worker = static_cast<CommercialControlsWorker*>(worker_);
    worker->moveToThread(&workerThread_);
    connect(&workerThread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &CommercialControlsController::loadRequested, worker,
            &CommercialControlsWorker::load, Qt::QueuedConnection);
    connect(this, &CommercialControlsController::consentStoreRequested, worker,
            &CommercialControlsWorker::storeConsent, Qt::QueuedConnection);
    connect(this, &CommercialControlsController::signOutRequested, worker,
            &CommercialControlsWorker::signOut, Qt::QueuedConnection);
    connect(this, &CommercialControlsController::deletionRequested, worker,
            &CommercialControlsWorker::deleteData, Qt::QueuedConnection);
    connect(worker, &CommercialControlsWorker::finished, this,
            &CommercialControlsController::finishOperation, Qt::QueuedConnection);
    workerThread_.start();
    setBusy(true);
    emit loadRequested(applicationStateRoot_, accountStateRoot_, ++generation_);
}

CommercialControlsController::~CommercialControlsController() {
    ++generation_;
    disconnect(worker_, nullptr, this, nullptr);
    disconnect(this, nullptr, worker_, nullptr);
    workerThread_.quit();
    workerThread_.wait();
}

void CommercialControlsController::setDiagnosticsConsent(bool consent) {
    if (busy_) {
        setStatusMessage(QStringLiteral("Account and privacy settings are busy."));
        return;
    }
    if (consent == diagnosticsConsent_) return;
    setStatusMessage({});
    setBusy(true);
    emit consentStoreRequested(applicationStateRoot_, accountStateRoot_, consent,
                               ++generation_);
}

void CommercialControlsController::signOut() {
    if (busy_) {
        setStatusMessage(QStringLiteral("Account and privacy settings are busy."));
        return;
    }
    setStatusMessage({});
    setBusy(true);
    emit signOutRequested(applicationStateRoot_, accountStateRoot_, ++generation_);
}

void CommercialControlsController::deleteLocalAccountData(bool confirmed) {
    if (!confirmed) {
        setStatusMessage(
            QStringLiteral("Confirm local account data deletion before continuing."));
        return;
    }
    if (busy_) {
        setStatusMessage(QStringLiteral("Account and privacy settings are busy."));
        return;
    }
    setStatusMessage({});
    setBusy(true);
    emit deletionRequested(applicationStateRoot_, accountStateRoot_, ++generation_);
}

void CommercialControlsController::finishOperation(
    quint64 generation, int operationValue, bool success, bool consent,
    const QString& error) {
    if (generation != generation_) return;
    const auto operation = static_cast<Operation>(operationValue);
    if (success) {
        if (operation == Operation::Load || operation == Operation::StoreConsent ||
            operation == Operation::DeleteData) {
            const bool nextConsent =
                operation == Operation::DeleteData ? false : consent;
            if (diagnosticsConsent_ != nextConsent) {
                diagnosticsConsent_ = nextConsent;
                emit diagnosticsConsentChanged();
            }
        }
        if (operation == Operation::SignOut) {
            setStatusMessage(QStringLiteral("Local account session cleared."));
        } else if (operation == Operation::DeleteData) {
            setStatusMessage(QStringLiteral("Local account data deleted."));
        } else {
            setStatusMessage({});
        }
    } else {
        setStatusMessage(error);
    }
    setBusy(false);
}

void CommercialControlsController::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void CommercialControlsController::setStatusMessage(QString message) {
    if (statusMessage_ == message) return;
    statusMessage_ = std::move(message);
    emit statusMessageChanged();
}

}  // namespace creator::app

#include "CommercialControlsController.moc"
