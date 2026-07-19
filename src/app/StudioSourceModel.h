#pragma once

#include "domain/StudioScene.h"

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <optional>
#include <vector>

namespace creator::app {

class StudioSourceModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)

public:
    enum Role {
        SourceIdRole = Qt::UserRole + 1,
        NameRole,
        RoleNameRole,
        PositionRole,
        EnabledRole,
        SelectedRole,
        HasTransformRole,
        TransformRole,
        SourceEnabledRole,
    };

    explicit StudioSourceModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] qulonglong revision() const noexcept { return revision_; }
    Q_INVOKABLE bool enabledForRole(const QString& roleName) const;
    Q_INVOKABLE QVariantMap transformForRole(const QString& roleName) const;

    void setScene(const domain::StudioScene& scene,
                  std::optional<domain::SourceId> selectedSourceId);
    void clear();

signals:
    void revisionChanged();

private:
    std::vector<domain::SceneSource> sources_;
    std::optional<domain::SourceId> selectedSourceId_;
    qulonglong revision_{0};
};

}  // namespace creator::app
