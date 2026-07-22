#pragma once

#include "edit_engine/EditEngineTypes.h"

#include <QAudioFormat>
#include <QObject>
#include <QString>

#include <cstdint>
#include <memory>

QT_BEGIN_NAMESPACE
class QAudioSink;
QT_END_NAMESPACE

namespace creator::app {

class PreviewAudioRingDevice;

/// Owns the Qt audio sink that makes editor-preview playback audible.
///
/// The edit engine hands up fully-mixed interleaved 32-bit float PCM
/// (edit_engine::PreviewAudioBlock); this class copies it into a BOUNDED ring
/// buffer that a QAudioSink pulls from at 48 kHz / 2ch / float. The ring never
/// grows past its fixed capacity (CLAUDE.md forbids infinite queues): on
/// overflow the incoming block is dropped and errorOccurred fires, and on
/// underrun the sink is fed silence so it stays active instead of stalling.
///
/// Qt Multimedia lives only in the application layer (CLAUDE.md 3/5). This class
/// is the whole of that surface for the editor preview; the engine and domain
/// layers only ever see the plain PreviewAudioBlock POD.
class EditorPreviewAudioOutput final : public QObject {
    Q_OBJECT
public:
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::uint32_t kChannels = 2;

    explicit EditorPreviewAudioOutput(QObject* parent = nullptr);
    ~EditorPreviewAudioOutput() override;

    /// Opens the sink against the default output device. Returns false and
    /// emits errorOccurred (never swallow, CLAUDE.md 9) when no device is
    /// available or the fixed format is unsupported. Idempotent while active.
    bool start();
    /// Stops the sink and drops all buffered audio.
    void stop();
    /// Drops buffered audio without tearing the sink down (used on seek so
    /// playback resumes cleanly from the new position).
    void flush();

    /// Copies one mixed block into the bounded ring. Rejects (returns false)
    /// blocks whose format does not match kSampleRate/kChannels, and drops the
    /// block on ring overflow rather than growing the queue.
    bool pushBlock(const edit_engine::PreviewAudioBlock& block);

    [[nodiscard]] bool active() const noexcept { return active_; }
    /// Per-channel samples currently buffered in the ring. Lets the controller
    /// pull only far enough ahead to keep the buffer bounded.
    [[nodiscard]] std::uint32_t queuedSamples() const;

signals:
    void errorOccurred(const QString& message);

private:
    QAudioFormat format_;
    std::unique_ptr<PreviewAudioRingDevice> device_;
    std::unique_ptr<QAudioSink> sink_;
    bool active_{false};
    bool overflowReported_{false};
};

}  // namespace creator::app
