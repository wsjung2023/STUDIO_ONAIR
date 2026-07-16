#pragma once

#include "domain/Timeline.h"

#include <QAbstractListModel>
#include <QHash>
#include <QVariant>

#include <optional>

namespace creator::app {

class TimelineTrackModel final : public QAbstractListModel {
public:
    enum Role {
        TrackIdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        EnabledRole,
        LockedRole,
        ClipsRole,
    };

    explicit TimelineTrackModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setTimeline(domain::Timeline timeline);

private:
    std::optional<domain::Timeline> timeline_;
};

}  // namespace creator::app
