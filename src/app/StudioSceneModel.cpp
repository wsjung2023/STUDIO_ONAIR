#include "app/StudioSceneModel.h"

#include <QString>

#include <cstddef>
#include <utility>

namespace creator::app {

StudioSceneModel::StudioSceneModel(QObject* parent)
    : QAbstractListModel(parent) {}

int StudioSceneModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(scenes_.size());
}

QVariant StudioSceneModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= scenes_.size()) {
        return {};
    }
    const auto& scene = scenes_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return QString::fromStdString(scene.name());
    case SceneIdRole:
        return QString::fromStdString(scene.id().value());
    case PositionRole:
        return scene.position();
    case ActiveRole:
        return activeSceneId_.has_value() && *activeSceneId_ == scene.id();
    case SelectedRole:
        return selectedSceneId_.has_value() && *selectedSceneId_ == scene.id();
    case SourceCountRole:
        return static_cast<int>(scene.sources().size());
    default:
        return {};
    }
}

QHash<int, QByteArray> StudioSceneModel::roleNames() const {
    return {{SceneIdRole, "sceneId"},
            {NameRole, "name"},
            {PositionRole, "position"},
            {ActiveRole, "active"},
            {SelectedRole, "selected"},
            {SourceCountRole, "sourceCount"}};
}

QString StudioSceneModel::sceneIdAt(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= scenes_.size()) return {};
    return QString::fromStdString(scenes_[static_cast<std::size_t>(row)].id().value());
}

void StudioSceneModel::setScenes(
    std::vector<domain::StudioScene> scenes,
    std::optional<domain::SceneId> activeSceneId,
    std::optional<domain::SceneId> selectedSceneId) {
    if (scenes_ == scenes && activeSceneId_ == activeSceneId &&
        selectedSceneId_ == selectedSceneId) {
        return;
    }
    beginResetModel();
    scenes_ = std::move(scenes);
    activeSceneId_ = std::move(activeSceneId);
    selectedSceneId_ = std::move(selectedSceneId);
    endResetModel();
}

}  // namespace creator::app
