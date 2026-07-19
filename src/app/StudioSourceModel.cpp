#include "app/StudioSourceModel.h"

#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <utility>

namespace creator::app {
namespace {

QVariant transformValue(const domain::SceneSource& source) {
    if (!source.transform().has_value()) return {};
    const auto& value = *source.transform();
    return QVariantMap{{QStringLiteral("x"), value.x()},
                       {QStringLiteral("y"), value.y()},
                       {QStringLiteral("width"), value.width()},
                       {QStringLiteral("height"), value.height()},
                       {QStringLiteral("scaleX"), value.scaleX()},
                       {QStringLiteral("scaleY"), value.scaleY()},
                       {QStringLiteral("rotationDegrees"),
                        value.rotationDegrees()},
                       {QStringLiteral("cropLeft"), value.cropLeft()},
                       {QStringLiteral("cropTop"), value.cropTop()},
                       {QStringLiteral("cropRight"), value.cropRight()},
                       {QStringLiteral("cropBottom"), value.cropBottom()},
                       {QStringLiteral("opacity"), value.opacity()},
                       {QStringLiteral("zOrder"), value.zOrder()}};
}

}  // namespace

StudioSourceModel::StudioSourceModel(QObject* parent)
    : QAbstractListModel(parent) {}

int StudioSourceModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(sources_.size());
}

QVariant StudioSourceModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= sources_.size()) {
        return {};
    }
    const auto& source = sources_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return QString::fromStdString(source.name());
    case SourceIdRole:
        return QString::fromStdString(source.id().value());
    case RoleNameRole:
        return QString::fromUtf8(domain::studioSourceRoleName(source.role()));
    case PositionRole:
        return source.position();
    case EnabledRole:
    case SourceEnabledRole:
        return source.enabled();
    case SelectedRole:
        return selectedSourceId_.has_value() &&
               *selectedSourceId_ == source.id();
    case HasTransformRole:
        return source.transform().has_value();
    case TransformRole:
        return transformValue(source);
    default:
        return {};
    }
}

QHash<int, QByteArray> StudioSourceModel::roleNames() const {
    return {{SourceIdRole, "sourceId"},
            {NameRole, "name"},
            {RoleNameRole, "role"},
            {PositionRole, "position"},
            {EnabledRole, "enabled"},
            {SelectedRole, "selected"},
            {HasTransformRole, "hasTransform"},
            {TransformRole, "transform"},
            {SourceEnabledRole, "sourceEnabled"}};
}

bool StudioSourceModel::enabledForRole(const QString& roleName) const {
    for (const auto& source : sources_) {
        if (QString::fromUtf8(domain::studioSourceRoleName(source.role())) ==
            roleName) {
            return source.enabled();
        }
    }
    return false;
}

QVariantMap StudioSourceModel::transformForRole(const QString& roleName) const {
    for (const auto& source : sources_) {
        if (QString::fromUtf8(domain::studioSourceRoleName(source.role())) ==
            roleName) {
            return transformValue(source).toMap();
        }
    }
    return {};
}

void StudioSourceModel::setScene(
    const domain::StudioScene& scene,
    std::optional<domain::SourceId> selectedSourceId) {
    if (sources_ == scene.sources() &&
        selectedSourceId_ == selectedSourceId) {
        return;
    }
    beginResetModel();
    sources_ = scene.sources();
    selectedSourceId_ = std::move(selectedSourceId);
    endResetModel();
    ++revision_;
    emit revisionChanged();
}

void StudioSourceModel::clear() {
    if (sources_.empty() && !selectedSourceId_.has_value()) return;
    beginResetModel();
    sources_.clear();
    selectedSourceId_.reset();
    endResetModel();
    ++revision_;
    emit revisionChanged();
}

}  // namespace creator::app
