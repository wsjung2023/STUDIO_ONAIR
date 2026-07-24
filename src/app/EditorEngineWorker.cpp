#include "app/EditorEngineWorker.h"

#include "core/AppError.h"
#include "media/MediaTypes.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

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

void EditorEngineWorker::requestAudio(quint64 generation, quint64 commandId,
                                      core::TimestampNs position,
                                      quint32 frequency, quint32 channels,
                                      quint32 samples) {
    // The MLT mixer returns exactly one timeline frame of audio per call, so a
    // per-frame round-trip to the UI thread only just keeps up at 30 fps and
    // any hiccup starves the sink (audible stutter). Accumulate several frames
    // into one block here, off the UI thread, so each round-trip delivers a
    // comfortable chunk of lookahead and the sink never underruns in steady
    // state. `samples` is the total per-channel target for this pull.
    const std::int64_t startNs = position.time_since_epoch().count();
    const auto target = static_cast<std::size_t>(samples) *
                        static_cast<std::size_t>(channels);
    const int perCall = static_cast<int>(
        std::min<quint32>(frequency, std::max<quint32>(samples, 1U)));
    std::vector<float> accumulated;
    accumulated.reserve(target);
    core::TimestampNs cursor = position;
    QString lastError;
    int guard = 0;
    while (accumulated.size() < target && guard++ < 1024) {
        auto result = engine_->requestMixedAudio(cursor, static_cast<int>(frequency),
                                                 static_cast<int>(channels), perCall);
        if (!result.hasValue()) {
            lastError = QString::fromStdString(result.error().message());
            break;  // e.g. reached end of timeline; emit what we already have
        }
        const auto& block = result.value();
        if (block.interleaved.empty()) break;
        accumulated.insert(accumulated.end(), block.interleaved.begin(),
                           block.interleaved.end());
        const std::int64_t frameSamples =
            static_cast<std::int64_t>(block.interleaved.size() / channels);
        if (frameSamples <= 0) break;
        // Round UP, matching frameToTimestamp's convention. Truncating would
        // leave the cursor a fraction of a nanosecond inside the frame just
        // pulled (timestampToFrame floors), re-pulling it as a duplicate and
        // skipping the last frame of every block -- an audible ~one-frame
        // glitch at each block boundary.
        const std::int64_t rate = static_cast<std::int64_t>(frequency);
        cursor = cursor + core::DurationNs{
                              (frameSamples * 1'000'000'000LL + rate - 1) / rate};
    }
    if (accumulated.empty()) {
        emit audioCompleted(
            generation, commandId, false,
            lastError.isEmpty() ? QStringLiteral("no mixed audio produced")
                                : lastError,
            startNs, {});
        return;
    }
    const auto byteCount =
        static_cast<qsizetype>(accumulated.size() * sizeof(float));
    QByteArray pcm{reinterpret_cast<const char*>(accumulated.data()), byteCount};
    emit audioCompleted(generation, commandId, true, {}, startNs,
                        std::move(pcm));
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
