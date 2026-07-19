#pragma once

#include "platform_release/EntitlementPolicy.h"

#include <QObject>
#include <QString>
#include <QThread>

#include <filesystem>

namespace creator::app {

class CommercialControlsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString entitlementState READ entitlementState CONSTANT)
    Q_PROPERTY(QString entitlementReason READ entitlementReason CONSTANT)
    Q_PROPERTY(bool diagnosticsConsent READ diagnosticsConsent NOTIFY diagnosticsConsentChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

public:
    explicit CommercialControlsController(
        platform_release::EntitlementDecision decision,
        QObject* parent = nullptr);
    CommercialControlsController(
        std::filesystem::path applicationStateRoot,
        std::filesystem::path accountStateRoot,
        platform_release::EntitlementDecision decision,
        QObject* parent = nullptr);
    ~CommercialControlsController() override;

    [[nodiscard]] QString entitlementState() const { return entitlementState_; }
    [[nodiscard]] QString entitlementReason() const { return entitlementReason_; }
    [[nodiscard]] bool diagnosticsConsent() const noexcept {
        return diagnosticsConsent_;
    }
    [[nodiscard]] bool busy() const noexcept { return busy_; }
    [[nodiscard]] QString statusMessage() const { return statusMessage_; }

    Q_INVOKABLE void setDiagnosticsConsent(bool consent);
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void deleteLocalAccountData(bool confirmed);

signals:
    void diagnosticsConsentChanged();
    void busyChanged();
    void statusMessageChanged();

    void loadRequested(QString applicationRoot, QString accountRoot,
                       quint64 generation);
    void consentStoreRequested(QString applicationRoot, QString accountRoot,
                               bool consent, quint64 generation);
    void signOutRequested(QString applicationRoot, QString accountRoot,
                          quint64 generation);
    void deletionRequested(QString applicationRoot, QString accountRoot,
                           quint64 generation);

private:
    void finishOperation(quint64 generation, int operation, bool success,
                         bool consent, const QString& error);
    void setBusy(bool busy);
    void setStatusMessage(QString message);

    QString applicationStateRoot_;
    QString accountStateRoot_;
    QString entitlementState_;
    QString entitlementReason_;
    QString statusMessage_;
    QObject* worker_{nullptr};
    QThread workerThread_;
    quint64 generation_{0};
    bool diagnosticsConsent_{false};
    bool busy_{false};
};

}  // namespace creator::app
