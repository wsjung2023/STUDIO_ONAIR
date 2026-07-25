#include "app/EditorPreviewAudioOutput.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace creator::app {

/// Bounded, thread-safe FIFO that a QAudioSink pulls from.
///
/// The sink pulls (readData) from its own backend context while the editor
/// pushes (writeData) from the UI thread, so every access is guarded by a
/// mutex. The buffer is a fixed-capacity ring: writes past capacity are refused
/// (the caller drops the block, never grows the queue) and reads past the
/// available data are padded with silence so the sink stays in ActiveState
/// instead of stalling on a momentary underrun.
class PreviewAudioRingDevice final : public QIODevice {
public:
    explicit PreviewAudioRingDevice(std::size_t capacityBytes)
        : buffer_(capacityBytes == 0 ? 1 : capacityBytes),
          capacity_(capacityBytes == 0 ? 1 : capacityBytes) {}

    [[nodiscard]] bool isSequential() const override { return true; }

    // Always reports data ready so the sink keeps pulling at its own cadence;
    // readData supplies silence whenever the ring has actually drained.
    [[nodiscard]] qint64 bytesAvailable() const override {
        return static_cast<qint64>(capacity_);
    }

    void clearBuffer() {
        const std::lock_guard<std::mutex> lock{mutex_};
        head_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::size_t bufferedBytes() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return size_;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (maxSize <= 0 || data == nullptr) return 0;
        const auto want = static_cast<std::size_t>(maxSize);
        const std::lock_guard<std::mutex> lock{mutex_};
        std::size_t produced = 0;
        while (produced < want && size_ > 0) {
            const std::size_t chunk =
                std::min({want - produced, size_, capacity_ - head_});
            std::memcpy(data + produced, buffer_.data() + head_, chunk);
            head_ = (head_ + chunk) % capacity_;
            size_ -= chunk;
            produced += chunk;
        }
        if (produced < want) {
            std::memset(data + produced, 0, want - produced);
            produced = want;
        }
        return static_cast<qint64>(produced);
    }

    qint64 writeData(const char* data, qint64 maxSize) override {
        if (maxSize <= 0 || data == nullptr) return 0;
        const auto want = static_cast<std::size_t>(maxSize);
        const std::lock_guard<std::mutex> lock{mutex_};
        std::size_t consumed = 0;
        while (consumed < want && size_ < capacity_) {
            const std::size_t tail = (head_ + size_) % capacity_;
            const std::size_t chunk =
                std::min({want - consumed, capacity_ - size_, capacity_ - tail});
            std::memcpy(buffer_.data() + tail, data + consumed, chunk);
            size_ += chunk;
            consumed += chunk;
        }
        return static_cast<qint64>(consumed);
    }

private:
    mutable std::mutex mutex_;
    std::vector<char> buffer_;
    std::size_t capacity_;
    std::size_t head_{0};
    std::size_t size_{0};
};

namespace {

// The ring buffers roughly one second of preview audio; the controller only
// tops it up when it drains below its ~2/3 s pull target, so it never fills
// fully in steady state and stays a hard bound on latency and memory. The
// headroom above the pull target absorbs momentary worker-round-trip delays so
// the sink does not underrun (audible stutter).
constexpr qint64 kRingCapacityMicroseconds = 3'000'000;

}  // namespace

EditorPreviewAudioOutput::EditorPreviewAudioOutput(QObject* parent)
    : QObject(parent) {
    format_.setSampleRate(static_cast<int>(kSampleRate));
    format_.setChannelCount(static_cast<int>(kChannels));
    format_.setSampleFormat(QAudioFormat::Float);
}

EditorPreviewAudioOutput::~EditorPreviewAudioOutput() { stop(); }

bool EditorPreviewAudioOutput::start() {
    if (active_) return true;

    // Headless/test seam: when CS_DISABLE_PREVIEW_AUDIO is set, never open a real
    // audio device. Unit tests for editor playback/command serialization must be
    // deterministic regardless of whether the host has an audio device -- an
    // opened device pulls audio continuously, which perturbs call sequences and
    // adds a slow, flaky device-open. Video playback continues without audio, as
    // it does when no device is present.
    if (qEnvironmentVariableIsSet("CS_DISABLE_PREVIEW_AUDIO")) {
        return false;
    }

    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        emit errorOccurred(QStringLiteral(
            "No audio output device is available for editor preview playback"));
        return false;
    }
    if (!device.isFormatSupported(format_)) {
        emit errorOccurred(QStringLiteral(
            "The default audio output does not support 48 kHz stereo float "
            "preview audio"));
        return false;
    }

    const auto capacityBytes =
        static_cast<std::size_t>(std::max<qint64>(
            format_.bytesForDuration(kRingCapacityMicroseconds), 1));
    device_ = std::make_unique<PreviewAudioRingDevice>(capacityBytes);
    if (!device_->open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
        emit errorOccurred(
            QStringLiteral("Could not open the editor preview audio buffer"));
        device_.reset();
        return false;
    }

    sink_ = std::make_unique<QAudioSink>(device, format_);
    connect(sink_.get(), &QAudioSink::stateChanged, this,
            [this](QAudio::State state) {
                static_cast<void>(state);
                if (!sink_) return;
                const QAudio::Error error = sink_->error();
                // UnderrunError is expected transient starvation; readData feeds
                // silence through it, so only real faults are surfaced.
                if (error != QAudio::NoError &&
                    error != QAudio::UnderrunError) {
                    emit errorOccurred(QStringLiteral(
                        "Editor preview audio output failed"));
                }
            });

    sink_->start(device_.get());
    const QAudio::Error error = sink_->error();
    if (error != QAudio::NoError && error != QAudio::UnderrunError) {
        emit errorOccurred(
            QStringLiteral("Could not start editor preview audio output"));
        sink_.reset();
        device_->close();
        device_.reset();
        return false;
    }

    overflowReported_ = false;
    active_ = true;
    return true;
}

void EditorPreviewAudioOutput::stop() {
    if (sink_) {
        sink_->stop();
        sink_.reset();
    }
    if (device_) {
        device_->close();
        device_.reset();
    }
    active_ = false;
    overflowReported_ = false;
}

void EditorPreviewAudioOutput::flush() {
    if (device_) device_->clearBuffer();
}

bool EditorPreviewAudioOutput::pushBlock(
    const edit_engine::PreviewAudioBlock& block) {
    if (!active_ || !device_) return false;
    if (block.frequency != kSampleRate || block.channels != kChannels) {
        emit errorOccurred(QStringLiteral(
            "Editor preview audio block did not match 48 kHz stereo float"));
        return false;
    }
    if (block.interleaved.empty()) return true;

    const auto bytes =
        static_cast<qint64>(block.interleaved.size() * sizeof(float));
    const char* raw = reinterpret_cast<const char*>(block.interleaved.data());
    const qint64 accepted = device_->write(raw, bytes);
    if (accepted < bytes) {
        if (!overflowReported_) {
            overflowReported_ = true;
            emit errorOccurred(QStringLiteral(
                "Editor preview audio buffer overflowed; dropping audio to stay "
                "bounded"));
        }
        return false;
    }
    return true;
}

std::uint32_t EditorPreviewAudioOutput::queuedSamples() const {
    if (!device_) return 0;
    constexpr std::size_t bytesPerFrame = kChannels * sizeof(float);
    return static_cast<std::uint32_t>(device_->bufferedBytes() / bytesPerFrame);
}

}  // namespace creator::app
