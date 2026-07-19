#include "app/GeneratedOverlayCache.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::app {
namespace {

namespace fs = std::filesystem;

using core::AppError;
using core::ErrorCode;
using core::Result;
using edit_engine::GeneratedOverlayDescriptor;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidArgument, std::move(message)};
}

AppError ioFailure(std::string message) {
    return AppError{ErrorCode::IoFailure, std::move(message)};
}

std::mutex& overlayCacheMutex() {
    static std::mutex mutex;
    return mutex;
}

QString qPath(const fs::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    const auto utf8 = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(utf8.data()),
                             static_cast<qsizetype>(utf8.size()));
#endif
}

bool hasReparsePoint(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
#endif
}

bool isInside(const fs::path& root, const fs::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *rootPart != *candidatePart) {
            return false;
        }
    }
    return true;
}

Result<fs::path> prepareGeneratedDirectory(const fs::path& packageRoot) {
    std::error_code error;
    const auto rootStatus = fs::symlink_status(packageRoot, error);
    if (error || !fs::is_directory(rootStatus) ||
        fs::is_symlink(rootStatus) || hasReparsePoint(packageRoot)) {
        return invalid("generated overlay package root is not a safe directory");
    }
    const auto canonicalRoot = fs::canonical(packageRoot, error);
    if (error) return ioFailure("generated overlay package root cannot be resolved");

    fs::path current = packageRoot;
    for (const auto* component : {"cache", "generated"}) {
        current /= component;
        auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            fs::create_directory(current, error);
            if (error) {
                return ioFailure(
                    "generated overlay cache directory cannot be created");
            }
            status = fs::symlink_status(current, error);
        }
        if (error || !fs::is_directory(status) || fs::is_symlink(status) ||
            hasReparsePoint(current)) {
            return invalid(
                "generated overlay cache directory is not a safe directory");
        }
        const auto canonicalCurrent = fs::canonical(current, error);
        if (error || !isInside(canonicalRoot, canonicalCurrent)) {
            return invalid("generated overlay cache escapes the project package");
        }
    }
    return current;
}

Result<void> validateRasterLocation(const fs::path& packageRoot,
                                    const fs::path& target) {
    std::error_code error;
    const auto canonicalRoot = fs::canonical(packageRoot, error);
    if (error) return ioFailure("generated overlay package root cannot be resolved");
    const auto parentStatus = fs::symlink_status(target.parent_path(), error);
    if (error || !fs::is_directory(parentStatus) ||
        fs::is_symlink(parentStatus) || hasReparsePoint(target.parent_path())) {
        return invalid("generated overlay target parent is not safe");
    }
    const auto canonicalParent = fs::canonical(target.parent_path(), error);
    if (error || !isInside(canonicalRoot, canonicalParent)) {
        return invalid("generated overlay target escapes the project package");
    }
    if (target.parent_path().filename() != "generated" ||
        target.parent_path().parent_path().filename() != "cache" ||
        target.extension() != ".png") {
        return invalid("generated overlay target is outside cache/generated");
    }
    return core::ok();
}

class DirectoryWriteGuard final {
public:
    DirectoryWriteGuard(const DirectoryWriteGuard&) = delete;
    DirectoryWriteGuard& operator=(const DirectoryWriteGuard&) = delete;

 #ifdef _WIN32
    DirectoryWriteGuard(DirectoryWriteGuard&& other) noexcept
        : handles_(std::move(other.handles_))
    {}
 #else
    DirectoryWriteGuard(DirectoryWriteGuard&&) noexcept = default;
 #endif

    ~DirectoryWriteGuard() {
#ifdef _WIN32
        for (const HANDLE handle : handles_) CloseHandle(handle);
#endif
    }

    [[nodiscard]] static Result<DirectoryWriteGuard> acquire(
        const fs::path& packageRoot, const fs::path& directory) {
#ifdef _WIN32
        std::vector<HANDLE> handles;
        const std::array directories{
            packageRoot, packageRoot / "cache", directory};
        for (const auto& guardedDirectory : directories) {
            HANDLE handle = CreateFileW(
                guardedDirectory.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                for (const HANDLE opened : handles) CloseHandle(opened);
                return ioFailure(
                    "generated overlay directory handle cannot be acquired");
            }
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                              &attributes,
                                              sizeof(attributes)) ||
                (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
                    0) {
                CloseHandle(handle);
                for (const HANDLE opened : handles) CloseHandle(opened);
                return invalid(
                    "generated overlay directory handle is a reparse point");
            }
            handles.push_back(handle);
        }
        const HANDLE handle = handles.back();
        const DWORD required = GetFinalPathNameByHandleW(
            handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (required == 0) {
            for (const HANDLE opened : handles) CloseHandle(opened);
            return ioFailure(
                "generated overlay directory handle cannot be resolved");
        }
        std::wstring buffer(required, L'\0');
        const DWORD written = GetFinalPathNameByHandleW(
            handle, buffer.data(), required,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0 || written >= required) {
            for (const HANDLE opened : handles) CloseHandle(opened);
            return ioFailure(
                "generated overlay directory handle cannot be resolved");
        }
        buffer.resize(written);
        constexpr std::wstring_view kDevicePrefix = L"\\\\?\\";
        constexpr std::wstring_view kUncPrefix = L"\\\\?\\UNC\\";
        if (buffer.starts_with(kUncPrefix)) {
            buffer = L"\\\\" + buffer.substr(kUncPrefix.size());
        } else if (buffer.starts_with(kDevicePrefix)) {
            buffer.erase(0, kDevicePrefix.size());
        }
        std::error_code error;
        const auto canonicalRoot = fs::canonical(packageRoot, error);
        if (error) {
            for (const HANDLE opened : handles) CloseHandle(opened);
            return ioFailure(
                "generated overlay package root cannot be resolved");
        }
        const auto canonicalDirectory = fs::canonical(fs::path{buffer}, error);
        if (error || !isInside(canonicalRoot, canonicalDirectory)) {
            for (const HANDLE opened : handles) CloseHandle(opened);
            return invalid(
                "generated overlay directory handle escapes the package");
        }
        return DirectoryWriteGuard{std::move(handles)};
#else
        auto validated = validateRasterLocation(
            packageRoot, directory / "guard.png");
        if (!validated.hasValue()) return validated.error();
        return DirectoryWriteGuard{};
#endif
    }

private:
#ifdef _WIN32
    explicit DirectoryWriteGuard(std::vector<HANDLE> handles)
        : handles_(std::move(handles)) {}
    std::vector<HANDLE> handles_;
#else
    DirectoryWriteGuard() = default;
#endif
};

Result<void> cleanupTemporaryFiles(const fs::path& directory) {
    std::error_code error;
    for (fs::directory_iterator entries{directory, error}, end;
         !error && entries != end; entries.increment(error)) {
        if (entries->path().extension() != ".tmp") continue;
        const auto status = entries->symlink_status(error);
        if (error) break;
        if (fs::is_regular_file(status) && !fs::is_symlink(status) &&
            !hasReparsePoint(entries->path())) {
            fs::remove(entries->path(), error);
            if (error) break;
        }
    }
    if (error) {
        return ioFailure("generated overlay temporary files cannot be cleaned");
    }
    return core::ok();
}

void appendUnsigned(QByteArray& payload, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        payload.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

void appendSigned(QByteArray& payload, std::int64_t value) {
    appendUnsigned(payload, std::bit_cast<std::uint64_t>(value));
}

void appendDouble(QByteArray& payload, double value) {
    appendUnsigned(payload, std::bit_cast<std::uint64_t>(value));
}

void appendBytes(QByteArray& payload, std::string_view value) {
    appendUnsigned(payload, value.size());
    payload.append(value.data(), static_cast<qsizetype>(value.size()));
}

void appendColor(QByteArray& payload, const domain::RgbaColor& color) {
    payload.append(static_cast<char>(color.red()));
    payload.append(static_cast<char>(color.green()));
    payload.append(static_cast<char>(color.blue()));
    payload.append(static_cast<char>(color.alpha()));
}

std::string resolveFontFamily(std::string_view requested) {
    const QString requestedName = QString::fromUtf8(
        requested.data(), static_cast<qsizetype>(requested.size()));
    const auto families = QFontDatabase::families();
    const auto exact = std::find_if(
        families.begin(), families.end(), [&](const QString& family) {
            return family.compare(requestedName, Qt::CaseInsensitive) == 0;
        });
    const QString selected =
        exact != families.end()
            ? *exact
            : QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    const QByteArray utf8 = selected.toUtf8();
    return std::string{utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

QColor color(const domain::RgbaColor& value) {
    return QColor{value.red(), value.green(), value.blue(), value.alpha()};
}

Qt::Alignment alignment(domain::TextAlignment value) {
    switch (value) {
        case domain::TextAlignment::Left:
            return Qt::AlignLeft;
        case domain::TextAlignment::Center:
            return Qt::AlignHCenter;
        case domain::TextAlignment::Right:
            return Qt::AlignRight;
    }
    return Qt::AlignHCenter;
}

void drawLaidOutText(QPainter& painter, const QString& text, const QFont& font,
                     QRectF bounds, Qt::Alignment horizontal,
                     const QColor& foreground, const QColor& background) {
    QTextOption option;
    option.setAlignment(horizontal);
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    QTextLayout layout{text, font};
    layout.setTextOption(option);
    layout.beginLayout();
    qreal height = 0.0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(bounds.width());
        line.setPosition(QPointF{0.0, height});
        height += line.height();
        if (height > bounds.height()) break;
    }
    layout.endLayout();
    const qreal contentHeight = std::min(height, bounds.height());
    const QPointF origin{bounds.left(),
                         bounds.top() + (bounds.height() - contentHeight) / 2.0};
    const QRectF backgroundBounds{bounds.left() - 8.0, origin.y() - 5.0,
                                  bounds.width() + 16.0,
                                  contentHeight + 10.0};
    if (background.alpha() != 0) painter.fillRect(backgroundBounds, background);
    painter.setPen(foreground);
    layout.draw(&painter, origin);
}

QImage renderTitle(const domain::TitlePayload& title, std::int32_t width,
                   std::int32_t height, const std::string& resolvedFamily) {
    QImage image{width, height, QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);
    QPainter painter{&image};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font{QString::fromUtf8(resolvedFamily)};
    font.setPixelSize(std::max(6, static_cast<int>(height * 0.09)));
    const qreal boxWidth = width * 0.82;
    const qreal boxHeight = height * 0.34;
    const qreal horizontalMargin = std::min(8.0, (width - boxWidth) / 2.0);
    const qreal verticalMargin = std::min(8.0, (height - boxHeight) / 2.0);
    const qreal left = std::clamp(title.x() * width - boxWidth / 2.0,
                                  horizontalMargin,
                                  width - boxWidth - horizontalMargin);
    const qreal top = std::clamp(title.y() * height - boxHeight / 2.0,
                                 verticalMargin,
                                 height - boxHeight - verticalMargin);
    drawLaidOutText(
        painter, QString::fromUtf8(title.text()), font,
        QRectF{left, top, boxWidth, boxHeight}, alignment(title.alignment()),
        color(title.foreground()), color(title.background()));
    painter.end();
    return image;
}

QImage renderCaption(const domain::CaptionCue& cue, std::int32_t width,
                     std::int32_t height,
                     const std::string& resolvedFamily) {
    QImage image{width, height, QImage::Format_ARGB32_Premultiplied};
    image.fill(Qt::transparent);
    QPainter painter{&image};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font{QString::fromUtf8(resolvedFamily)};
    font.setPixelSize(std::max(6, static_cast<int>(height * 0.065)));
    const qreal boxWidth = width * 0.88;
    const qreal boxHeight = height * 0.22;
    drawLaidOutText(
        painter, QString::fromUtf8(cue.text()), font,
        QRectF{(width - boxWidth) / 2.0, height * 0.72, boxWidth, boxHeight},
        Qt::AlignHCenter, QColor{255, 255, 255, 255},
        QColor{0, 0, 0, 180});
    painter.end();
    return image;
}

QByteArray titleKey(const domain::Clip& clip,
                    const std::string& resolvedFamily, std::int32_t width,
                    std::int32_t height, core::FrameRate frameRate) {
    QByteArray payload;
    appendBytes(payload, "creator-overlay-title-v2");
    appendBytes(payload, clip.id().value());
    appendSigned(payload,
                 clip.timelineRange().start().time_since_epoch().count());
    appendSigned(payload, clip.timelineRange().duration().count());
    const auto& title = *clip.titlePayload();
    appendBytes(payload, title.text());
    appendBytes(payload, title.fontFamily());
    appendBytes(payload, resolvedFamily);
    appendDouble(payload, title.x());
    appendDouble(payload, title.y());
    appendColor(payload, title.foreground());
    appendColor(payload, title.background());
    appendUnsigned(payload, static_cast<std::uint64_t>(title.alignment()));
    appendSigned(payload, width);
    appendSigned(payload, height);
    appendSigned(payload, frameRate.numerator());
    appendSigned(payload, frameRate.denominator());
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
}

QByteArray captionKey(const domain::Clip& clip, const domain::CaptionCue& cue,
                      const std::string& resolvedFamily, std::int32_t width,
                      std::int32_t height, core::FrameRate frameRate) {
    QByteArray payload;
    appendBytes(payload, "creator-overlay-caption-v2");
    appendBytes(payload, clip.id().value());
    appendBytes(payload, cue.id().value());
    appendSigned(payload,
                 clip.timelineRange().start().time_since_epoch().count());
    appendSigned(payload, clip.timelineRange().duration().count());
    appendSigned(payload, cue.startOffset().count());
    appendSigned(payload, cue.duration().count());
    appendBytes(payload, cue.text());
    appendBytes(payload, resolvedFamily);
    appendSigned(payload, width);
    appendSigned(payload, height);
    appendSigned(payload, frameRate.numerator());
    appendSigned(payload, frameRate.denominator());
    return QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
}

Result<void> ensureRaster(const fs::path& packageRoot, const fs::path& target,
                          QSize expectedSize,
                          const std::function<QImage()>& render,
                          const GeneratedOverlayCache::BeforeCommitHook&
                              beforeCommit) {
    if (auto validated = validateRasterLocation(packageRoot, target);
        !validated.hasValue()) {
        return validated.error();
    }
    auto directoryGuard =
        DirectoryWriteGuard::acquire(packageRoot, target.parent_path());
    if (!directoryGuard.hasValue()) return directoryGuard.error();
    std::error_code error;
    const auto status = fs::symlink_status(target, error);
    if (!error) {
        if (!fs::is_regular_file(status) || fs::is_symlink(status) ||
            hasReparsePoint(target)) {
            return invalid("generated overlay target is not a safe regular file");
        }
        QImageReader reader{qPath(target)};
        const QByteArray format = reader.format().toLower();
        const QImage existing = reader.read();
        if (format == "png" && !existing.isNull() &&
            existing.size() == expectedSize && fs::file_size(target, error) > 0 &&
            !error) {
            return core::ok();
        }
    } else if (error != std::errc::no_such_file_or_directory) {
        return ioFailure("generated overlay target cannot be inspected");
    }

    if (auto validated = validateRasterLocation(packageRoot, target);
        !validated.hasValue()) {
        return validated.error();
    }
    const QImage image = render();
    if (image.isNull() || image.size() != expectedSize ||
        image.format() != QImage::Format_ARGB32_Premultiplied) {
        return ioFailure("generated overlay rasterization failed");
    }

    QSaveFile file{qPath(target)};
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return ioFailure("generated overlay temporary file cannot be opened");
    }
    if (!image.save(&file, "PNG", 9)) {
        file.cancelWriting();
        return ioFailure("generated overlay PNG encoding failed");
    }
    if (beforeCommit) {
        auto allowed = beforeCommit(target);
        if (!allowed.hasValue()) {
            file.cancelWriting();
            return allowed.error();
        }
    }
    if (!file.commit()) {
        return ioFailure("generated overlay atomic commit failed");
    }
    return core::ok();
}

fs::path relativeRasterPath(const QByteArray& key) {
    return fs::path{"cache"} / "generated" /
           (key.toStdString() + ".png");
}

AppError overlayDiagnostic(const AppError& cause, const domain::Clip& clip,
                           const domain::CueId* cue,
                           const std::string& resolvedFamily) {
    std::string message = "generated overlay clip '" + clip.id().value() + "'";
    if (cue != nullptr) {
        message += " cue '" + cue->value() + "'";
    }
    message += " resolved font '" + resolvedFamily + "': " + cause.message();
    return AppError{cause.code(), std::move(message)};
}

}  // namespace

Result<GeneratedOverlayCacheResult> GeneratedOverlayCache::synchronize(
    const fs::path& packageRoot, const domain::Timeline& timeline,
    std::int32_t canvasWidth, std::int32_t canvasHeight,
    core::FrameRate frameRate) const {
#ifndef _WIN32
    return AppError{
        ErrorCode::UnsupportedVersion,
        "secure generated overlay cache publication is currently available only on Windows"};
#endif
    const std::scoped_lock synchronizationLock{overlayCacheMutex()};
    constexpr std::int32_t kMinimumDimension = 16;
    constexpr std::int32_t kMaximumDimension = 16'384;
    if (canvasWidth < kMinimumDimension || canvasHeight < kMinimumDimension ||
        canvasWidth > kMaximumDimension || canvasHeight > kMaximumDimension) {
        return invalid("generated overlay canvas dimensions are invalid");
    }
    auto directory = prepareGeneratedDirectory(packageRoot);
    if (!directory.hasValue()) return directory.error();
    auto cleanupGuard =
        DirectoryWriteGuard::acquire(packageRoot, directory.value());
    if (!cleanupGuard.hasValue()) return cleanupGuard.error();
    auto cleaned = cleanupTemporaryFiles(directory.value());
    if (!cleaned.hasValue()) return cleaned.error();

    GeneratedOverlayCacheResult result;
    for (const auto& track : timeline.tracks()) {
        if (!track.enabled()) continue;
        for (const auto& clip : track.clips()) {
            if (!clip.enabled() || clip.kind() == domain::ClipKind::Asset) {
                continue;
            }
            if (clip.kind() == domain::ClipKind::Title) {
                const auto& title = *clip.titlePayload();
                const auto resolved = resolveFontFamily(title.fontFamily());
                const auto key = titleKey(clip, resolved, canvasWidth,
                                          canvasHeight, frameRate);
                const auto relative = relativeRasterPath(key);
                const auto absolute = packageRoot / relative;
                auto written = ensureRaster(
                    packageRoot, absolute, QSize{canvasWidth, canvasHeight},
                    [&] {
                        return renderTitle(title, canvasWidth, canvasHeight,
                                           resolved);
                    },
                    beforeCommit_);
                if (!written.hasValue()) {
                    result.diagnostics.push_back(overlayDiagnostic(
                        written.error(), clip, nullptr, resolved));
                    continue;
                }
                auto descriptor = GeneratedOverlayDescriptor::create(
                    clip.id(), std::nullopt, relative, clip.timelineRange(),
                    resolved);
                if (!descriptor.hasValue()) {
                    result.diagnostics.push_back(overlayDiagnostic(
                        descriptor.error(), clip, nullptr, resolved));
                    continue;
                }
                result.descriptors.push_back(std::move(descriptor).value());
                continue;
            }
            const auto resolved = resolveFontFamily("Arial");
            for (const auto& cue : clip.captionCues()) {
                const auto key = captionKey(clip, cue, resolved, canvasWidth,
                                            canvasHeight, frameRate);
                const auto relative = relativeRasterPath(key);
                const auto absolute = packageRoot / relative;
                auto written = ensureRaster(
                    packageRoot, absolute, QSize{canvasWidth, canvasHeight},
                    [&] {
                        return renderCaption(cue, canvasWidth, canvasHeight,
                                             resolved);
                    },
                    beforeCommit_);
                if (!written.hasValue()) {
                    result.diagnostics.push_back(overlayDiagnostic(
                        written.error(), clip, &cue.id(), resolved));
                    continue;
                }
                auto range = domain::TimeRange::create(
                    clip.timelineRange().start() + cue.startOffset(),
                    cue.duration());
                if (!range.hasValue()) {
                    result.diagnostics.push_back(overlayDiagnostic(
                        range.error(), clip, &cue.id(), resolved));
                    continue;
                }
                auto descriptor = GeneratedOverlayDescriptor::create(
                    clip.id(), cue.id(), relative, range.value(), resolved);
                if (!descriptor.hasValue()) {
                    result.diagnostics.push_back(overlayDiagnostic(
                        descriptor.error(), clip, &cue.id(), resolved));
                    continue;
                }
                result.descriptors.push_back(std::move(descriptor).value());
            }
        }
    }
    return result;
}

}  // namespace creator::app
