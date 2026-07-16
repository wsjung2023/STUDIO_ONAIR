#include "app/TimelineTrackModel.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <utility>

namespace creator::app {
namespace {

QString trackKindName(domain::TrackKind kind) {
    switch (kind) {
        case domain::TrackKind::Video:
            return QStringLiteral("video");
        case domain::TrackKind::Audio:
            return QStringLiteral("audio");
        case domain::TrackKind::Title:
            return QStringLiteral("title");
        case domain::TrackKind::Caption:
            return QStringLiteral("caption");
    }
    return {};
}

QString clipKindName(domain::ClipKind kind) {
    switch (kind) {
        case domain::ClipKind::Asset:
            return QStringLiteral("asset");
        case domain::ClipKind::Title:
            return QStringLiteral("title");
        case domain::ClipKind::Caption:
            return QStringLiteral("caption");
    }
    return {};
}

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

QVariantMap clipDto(const domain::Clip& clip) {
    QVariantMap dto{
        {QStringLiteral("id"), QString::fromStdString(clip.id().value())},
        {QStringLiteral("kind"), clipKindName(clip.kind())},
        {QStringLiteral("mediaKind"), mediaKindName(clip.mediaKind())},
        {QStringLiteral("enabled"), clip.enabled()},
        {QStringLiteral("sourceStartNs"),
         QVariant::fromValue<qint64>(
             clip.sourceRange().start().time_since_epoch().count())},
        {QStringLiteral("sourceDurationNs"),
         QVariant::fromValue<qint64>(clip.sourceRange().duration().count())},
        {QStringLiteral("timelineStartNs"),
         QVariant::fromValue<qint64>(
             clip.timelineRange().start().time_since_epoch().count())},
        {QStringLiteral("timelineDurationNs"),
         QVariant::fromValue<qint64>(clip.timelineRange().duration().count())},
        {QStringLiteral("durationNs"),
         QVariant::fromValue<qint64>(clip.timelineRange().duration().count())},
    };
    if (clip.assetId().has_value()) {
        dto.insert(QStringLiteral("assetId"),
                   QString::fromStdString(clip.assetId()->value()));
    }
    if (clip.visualTransform().has_value()) {
        const auto& transform = *clip.visualTransform();
        dto.insert(QStringLiteral("x"), transform.x());
        dto.insert(QStringLiteral("y"), transform.y());
        dto.insert(QStringLiteral("width"), transform.width());
        dto.insert(QStringLiteral("height"), transform.height());
        dto.insert(QStringLiteral("scaleX"), transform.scaleX());
        dto.insert(QStringLiteral("scaleY"), transform.scaleY());
        dto.insert(QStringLiteral("rotationDegrees"),
                   transform.rotationDegrees());
        dto.insert(QStringLiteral("cropLeft"), transform.cropLeft());
        dto.insert(QStringLiteral("cropTop"), transform.cropTop());
        dto.insert(QStringLiteral("cropRight"), transform.cropRight());
        dto.insert(QStringLiteral("cropBottom"), transform.cropBottom());
        dto.insert(QStringLiteral("opacity"), transform.opacity());
        dto.insert(QStringLiteral("zOrder"), transform.zOrder());
    }
    if (clip.audioEnvelope().has_value()) {
        const auto& envelope = *clip.audioEnvelope();
        dto.insert(QStringLiteral("gainDb"), envelope.gainDb());
        dto.insert(QStringLiteral("fadeInNs"),
                   QVariant::fromValue<qint64>(envelope.fadeIn().count()));
        dto.insert(QStringLiteral("fadeOutNs"),
                   QVariant::fromValue<qint64>(envelope.fadeOut().count()));
    }
    return dto;
}

QVariantList clipDtos(const domain::Track& track) {
    QVariantList result;
    result.reserve(static_cast<qsizetype>(track.clips().size()));
    for (const auto& clip : track.clips()) result.push_back(clipDto(clip));
    return result;
}

}  // namespace

TimelineTrackModel::TimelineTrackModel(QObject* parent)
    : QAbstractListModel(parent) {}

int TimelineTrackModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !timeline_.has_value()) return 0;
    return static_cast<int>(timeline_->tracks().size());
}

QVariant TimelineTrackModel::data(const QModelIndex& index, int role) const {
    if (!timeline_.has_value() || !index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= timeline_->tracks().size()) {
        return {};
    }
    const auto& track =
        timeline_->tracks()[static_cast<std::size_t>(index.row())];
    switch (role) {
        case TrackIdRole:
            return QString::fromStdString(track.id().value());
        case NameRole:
            return QString::fromStdString(track.name());
        case KindRole:
            return trackKindName(track.kind());
        case EnabledRole:
            return track.enabled();
        case LockedRole:
            return track.locked();
        case ClipsRole:
            return clipDtos(track);
        default:
            return {};
    }
}

QHash<int, QByteArray> TimelineTrackModel::roleNames() const {
    return {{TrackIdRole, "trackId"}, {NameRole, "name"},
            {KindRole, "kind"},       {EnabledRole, "enabled"},
            {LockedRole, "locked"},  {ClipsRole, "clips"}};
}

void TimelineTrackModel::setTimeline(domain::Timeline timeline) {
    beginResetModel();
    timeline_ = std::move(timeline);
    endResetModel();
}

}  // namespace creator::app
