#pragma once

#include "domain/StudioScene.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>

#include <optional>
#include <vector>

namespace creator::app {

class StudioSceneModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(qulonglong revision READ revision NOTIFY revisionChanged)

public:
    enum Role {
        SceneIdRole = Qt::UserRole + 1,
        NameRole,
        PositionRole,
        ActiveRole,
        SelectedRole,
        SourceCountRole,
    };

    explicit StudioSceneModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] qulonglong revision() const noexcept { return revision_; }
    Q_INVOKABLE QString sceneIdAt(int row) const;

    void setScenes(std::vector<domain::StudioScene> scenes,
                   std::optional<domain::SceneId> activeSceneId,
                   std::optional<domain::SceneId> selectedSceneId);

signals:
    void revisionChanged();

private:
    std::vector<domain::StudioScene> scenes_;
    std::optional<domain::SceneId> activeSceneId_;
    std::optional<domain::SceneId> selectedSceneId_;
    qulonglong revision_{0};
};

}  // namespace creator::app
