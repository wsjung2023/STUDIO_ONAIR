#include "app/EditorEngineWorker.h"

#include "core/AppError.h"
#include "media/MediaTypes.h"

#include <limits>
#include <utility>

namespace creator::app {

EditorEngineWorker::EditorEngineWorker(
    std::unique_ptr<edit_engine::IEditEngine> engine)
    : engine_(std::move(engine)) {}

void EditorEngineWorker::load(quint64 generation, quint64 commandId,
                              edit_engine::TimelineSnapshot snapshot) {
    publish(generation, commandId, EditorEngineOperation::Load,
            engine_->load(snapshot));
}

void EditorEngineWorker::update(quint64 generation, quint64 commandId,
                                edit_engine::TimelineChangeSet change) {
    publish(generation, commandId, EditorEngineOperation::Update,
            engine_->update(change));
}

void EditorEngineWorker::play(quint64 generation, quint64 commandId) {
    publish(generation, commandId, EditorEngineOperation::Play, engine_->play());
}

void EditorEngineWorker::pause(quint64 generation, quint64 commandId) {
    publish(generation, commandId, EditorEngineOperation::Pause,
            engine_->pause());
}

void EditorEngineWorker::seek(quint64 generation, quint64 commandId,
                              core::TimestampNs position) {
    publish(generation, commandId, EditorEngineOperation::Seek,
            engine_->seek(position));
}

void EditorEngineWorker::requestFrame(quint64 generation, quint64 commandId,
                                      core::TimestampNs position) {
    auto result = engine_->requestFrame(position);
    if (!result.hasValue()) {
        emit frameCompleted(generation, commandId, false,
                            QString::fromStdString(result.error().message()), -1,
                            position.time_since_epoch().count(), {});
        return;
    }

    const auto& preview = result.value();
    const auto& frame = preview.frame();
    constexpr std::uint64_t kBytesPerPixel = 4;
    constexpr std::uint64_t kMaximumPreviewBytes = 512ULL * 1024ULL * 1024ULL;
    const std::uint64_t stride =
        static_cast<std::uint64_t>(frame.width) * kBytesPerPixel;
    const std::uint64_t byteCount = stride * frame.height;
    if (frame.pixelFormat != media::PixelFormat::Bgra8 ||
        frame.width == 0 || frame.height == 0 || !frame.platformHandle ||
        stride > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        byteCount == 0 || byteCount > kMaximumPreviewBytes) {
        emit frameCompleted(generation, commandId, false,
                            QStringLiteral("Edit engine returned an invalid BGRA preview frame"),
                            preview.revision().value(),
                            preview.position().time_since_epoch().count(), {});
        return;
    }

    const auto* pixels = static_cast<const uchar*>(frame.platformHandle.get());
    const QImage view{pixels, static_cast<int>(frame.width),
                      static_cast<int>(frame.height), static_cast<int>(stride),
                      QImage::Format_ARGB32};
    QImage detached = view.copy();
    if (detached.isNull()) {
        emit frameCompleted(generation, commandId, false,
                            QStringLiteral("Could not copy the edit preview frame"),
                            preview.revision().value(),
                            preview.position().time_since_epoch().count(), {});
        return;
    }
    emit frameCompleted(generation, commandId, true, {},
                        preview.revision().value(),
                        preview.position().time_since_epoch().count(),
                        std::move(detached));
}

void EditorEngineWorker::publish(quint64 generation, quint64 commandId,
                                 EditorEngineOperation operation,
                                 const core::Result<void>& result) {
    emit completed(generation, commandId, static_cast<int>(operation),
                   result.hasValue(),
                   result.hasValue()
                       ? QString{}
                       : QString::fromStdString(result.error().message()));
}

}  // namespace creator::app
