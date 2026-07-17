#pragma once

#include "domain/StudioScene.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>

#include <optional>
#include <vector>

namespace creator::app {

class StudioSourceModel final : public QAbstractListModel {
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
    };

    explicit StudioSourceModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setScene(const domain::StudioScene& scene,
                  std::optional<domain::SourceId> selectedSourceId);
    void clear();

private:
    std::vector<domain::SceneSource> sources_;
    std::optional<domain::SourceId> selectedSourceId_;
};

}  // namespace creator::app
