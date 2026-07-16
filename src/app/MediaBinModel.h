#pragma once

#include "domain/MediaAsset.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>

#include <vector>

namespace creator::app {

class MediaBinModel final : public QAbstractListModel {
public:
    enum Role {
        AssetIdRole = Qt::UserRole + 1,
        PackagePathRole,
        KindRole,
        DurationNsRole,
        AvailableRole,
        VideoMetadataRole,
        AudioMetadataRole,
    };

    explicit MediaBinModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setAssets(std::vector<domain::MediaAsset> assets);

private:
    std::vector<domain::MediaAsset> assets_;
};

}  // namespace creator::app
