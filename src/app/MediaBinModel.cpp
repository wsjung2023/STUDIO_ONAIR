#include "app/MediaBinModel.h"

#include <QString>
#include <QVariantMap>

#include <cstddef>
#include <utility>

namespace creator::app {
namespace {

QString mediaKindName(domain::MediaKind kind) {
    switch (kind) {
        case domain::MediaKind::Video:
            return QStringLiteral("video");
        case domain::MediaKind::Audio:
            return QStringLiteral("audio");
        case domain::MediaKind::Image:
            return QStringLiteral("image");
    }
    return {};
}

QVariant videoMetadata(const domain::MediaAsset& asset) {
    if (!asset.video().has_value()) return {};
    const auto& video = *asset.video();
    return QVariantMap{
        {QStringLiteral("width"), video.width},
        {QStringLiteral("height"), video.height},
        {QStringLiteral("frameRateNumerator"),
         static_cast<qlonglong>(video.frameRate.numerator())},
        {QStringLiteral("frameRateDenominator"),
         static_cast<qlonglong>(video.frameRate.denominator())},
    };
}

QVariant audioMetadata(const domain::MediaAsset& asset) {
    if (!asset.audio().has_value()) return {};
    const auto& audio = *asset.audio();
    return QVariantMap{{QStringLiteral("sampleRate"), audio.sampleRate},
                       {QStringLiteral("channels"), audio.channels}};
}

}  // namespace

MediaBinModel::MediaBinModel(QObject* parent) : QAbstractListModel(parent) {}

int MediaBinModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(assets_.size());
}

QVariant MediaBinModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= assets_.size()) {
        return {};
    }
    const auto& asset = assets_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case AssetIdRole:
            return QString::fromStdString(asset.id().value());
        case PackagePathRole:
            return QString::fromStdString(asset.relativePath());
        case KindRole:
            return mediaKindName(asset.kind());
        case DurationNsRole:
            return QVariant::fromValue<qint64>(asset.duration().count());
        case AvailableRole:
            return asset.availability() == domain::AssetAvailability::Available;
        case VideoMetadataRole:
            return videoMetadata(asset);
        case AudioMetadataRole:
            return audioMetadata(asset);
        default:
            return {};
    }
}

QHash<int, QByteArray> MediaBinModel::roleNames() const {
    return {{AssetIdRole, "assetId"},
            {PackagePathRole, "packagePath"},
            {KindRole, "kind"},
            {DurationNsRole, "durationNs"},
            {AvailableRole, "available"},
            {VideoMetadataRole, "videoMetadata"},
            {AudioMetadataRole, "audioMetadata"}};
}

void MediaBinModel::setAssets(std::vector<domain::MediaAsset> assets) {
    beginResetModel();
    assets_ = std::move(assets);
    endResetModel();
}

}  // namespace creator::app
