#include "app/RecentProjectRegistry.h"

#include "core/AppError.h"
#include "core/Uuid.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace creator::app {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError registryError(ErrorCode code, QString message) {
    return AppError{code, message.toStdString()};
}

}  // namespace

std::filesystem::path pathFromQString(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path{value.toStdWString()};
#else
    const QByteArray utf8 = value.toUtf8();
    std::u8string bytes;
    bytes.reserve(static_cast<std::size_t>(utf8.size()));
    for (const char byte : utf8) bytes.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{bytes};
#endif
}

QString qStringFromPath(const std::filesystem::path& value) {
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    const auto bytes = value.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<qsizetype>(bytes.size()));
#endif
}

RecentProjectRegistry::RecentProjectRegistry(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

Result<std::vector<RecentProject>> RecentProjectRegistry::load() const {
    QFile file{qStringFromPath(filePath_)};
    if (!file.exists()) return std::vector<RecentProject>{};
    if (!file.open(QIODevice::ReadOnly)) {
        return registryError(ErrorCode::IoFailure, QStringLiteral("cannot open recent projects"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return registryError(ErrorCode::ParseFailure,
                             QStringLiteral("recent projects file is malformed"));
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != 1 ||
        !root.value(QStringLiteral("projects")).isArray()) {
        return registryError(ErrorCode::ParseFailure,
                             QStringLiteral("recent projects version or rows are invalid"));
    }
    std::vector<RecentProject> result;
    for (const auto& value : root.value(QStringLiteral("projects")).toArray()) {
        if (!value.isObject()) {
            return registryError(ErrorCode::ParseFailure,
                                 QStringLiteral("recent project row is invalid"));
        }
        const QJsonObject row = value.toObject();
        const QString path = row.value(QStringLiteral("path")).toString();
        const QString opened = row.value(QStringLiteral("lastOpenedAt")).toString();
        auto timestamp = core::Utc::parseRfc3339(opened.toStdString());
        if (path.isEmpty() || !timestamp.hasValue()) {
            return registryError(ErrorCode::ParseFailure,
                                 QStringLiteral("recent project row has invalid fields"));
        }
        result.push_back(RecentProject{.path = pathFromQString(path),
                                       .lastOpenedAt = timestamp.value()});
        if (result.size() == 20) break;
    }
    return result;
}

Result<void> RecentProjectRegistry::remember(const std::filesystem::path& path,
                                             const core::Utc& openedAt) {
    auto loaded = load();
    std::vector<RecentProject> projects;
    if (loaded.hasValue()) {
        projects = std::move(loaded).value();
    } else if (loaded.error().code() == ErrorCode::ParseFailure) {
        std::filesystem::path backup = filePath_;
        backup += ".corrupt-";
        backup += core::generateUuidV4();
        if (!QFile::rename(qStringFromPath(filePath_), qStringFromPath(backup))) {
            return registryError(ErrorCode::IoFailure,
                                 QStringLiteral("cannot back up malformed recent projects"));
        }
    } else {
        return loaded.error();
    }

    std::vector<RecentProject> deduplicated;
    for (auto& project : projects) {
        const bool duplicate = std::any_of(
            deduplicated.begin(), deduplicated.end(),
            [&](const RecentProject& item) { return item.path == project.path; });
        if (!duplicate && project.path != path) deduplicated.push_back(std::move(project));
    }
    projects = std::move(deduplicated);
    projects.insert(projects.begin(), RecentProject{.path = path, .lastOpenedAt = openedAt});
    if (projects.size() > 20) {
        projects.erase(projects.begin() + 20, projects.end());
    }

    const QString parent = QFileInfo{qStringFromPath(filePath_)}.absolutePath();
    if (!QDir{}.mkpath(parent)) {
        return registryError(ErrorCode::IoFailure,
                             QStringLiteral("cannot create recent projects directory"));
    }
    QJsonArray rows;
    for (const auto& project : projects) {
        rows.append(QJsonObject{{QStringLiteral("path"), qStringFromPath(project.path)},
                                {QStringLiteral("lastOpenedAt"),
                                 QString::fromStdString(project.lastOpenedAt.toRfc3339())}});
    }
    const QJsonDocument document{QJsonObject{{QStringLiteral("version"), 1},
                                              {QStringLiteral("projects"), rows}}};
    const QByteArray bytes = document.toJson();
    QSaveFile file{qStringFromPath(filePath_)};
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return registryError(ErrorCode::IoFailure,
                             QStringLiteral("cannot save recent projects"));
    }
    return core::ok();
}

}  // namespace creator::app
