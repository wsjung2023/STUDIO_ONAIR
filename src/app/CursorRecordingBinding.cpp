#include "app/CursorRecordingBinding.h"

#include "cursor/CursorNdjsonSink.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace creator::app {
namespace {

constexpr std::size_t kDrainBatch = 1024;

QString fromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString fromPath(const std::filesystem::path& path) {
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::string safeComponent(const std::string& value) {
    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

class TakeRelativeCursorSource final : public cursor::ICursorSource {
public:
    TakeRelativeCursorSource(std::unique_ptr<cursor::ICursorSource> source,
                             core::TimestampNs origin)
        : source_(std::move(source)), origin_(origin) {}

    [[nodiscard]] std::optional<cursor::RawCursorSample> poll() override {
        auto sample = source_->poll();
        if (!sample) return std::nullopt;
        std::visit(
            [this](auto& raw) {
                raw.tNs = core::TimestampNs{
                    std::max(core::DurationNs::zero(), raw.tNs - origin_)};
            },
            *sample);
        return sample;
    }

    [[nodiscard]] std::optional<core::AppError> error() const override {
        return source_->error();
    }

private:
    std::unique_ptr<cursor::ICursorSource> source_;
    core::TimestampNs origin_;
};

const LiveCaptureSource* firstScreenSource(const LiveRecordingStart& start) {
    const auto found = std::find_if(
        start.sources.begin(), start.sources.end(), [](const auto& source) {
            return source.role == recorder::TrackRole::Screen;
        });
    return found == start.sources.end() ? nullptr : &*found;
}

}  // namespace

CursorRecordingBinding::CursorRecordingBinding(
    LiveRecordingController& recording, SourceFactory sourceFactory,
    QObject* parent)
    : QObject(parent), recording_(&recording),
      sourceFactory_(std::move(sourceFactory)) {
    timer_.setInterval(8);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &CursorRecordingBinding::poll);
    connect(&recording, &LiveRecordingController::recordingChanged,
            this, &CursorRecordingBinding::handleRecordingChanged);
    if (!available()) {
        statusMessage_ = tr("Cursor data is unavailable on this platform");
    }
    handleRecordingChanged();
}

CursorRecordingBinding::~CursorRecordingBinding() {
    timer_.stop();
    finish();
}

void CursorRecordingBinding::handleRecordingChanged() {
    if (recording_ && recording_->isRecording()) {
        if (!active_ && pump_ == nullptr) {
            const auto* start = recording_->activeRecordingStart();
            if (start != nullptr) begin(*start);
        }
        return;
    }
    finish();
}

void CursorRecordingBinding::begin(const LiveRecordingStart& start) {
    eventCount_ = 0;
    outputPath_.clear();
    partPath_.clear();
    finalPath_.clear();
    if (!available()) {
        setStatus(tr("Cursor data is unavailable on this platform"));
        return;
    }
    const auto* screen = firstScreenSource(start);
    if (screen == nullptr) {
        setStatus(tr("No screen source; cursor data was not started"));
        return;
    }

    const auto directory = start.packagePath / "telemetry" / "cursor";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        fail(core::AppError{core::ErrorCode::IoFailure,
                            "could not create cursor telemetry directory: " +
                                error.message()});
        return;
    }
    const auto filename = safeComponent(start.sessionId.value()) + "-" +
                          safeComponent(screen->sourceId.value()) + ".ndjson";
    finalPath_ = directory / filename;
    partPath_ = finalPath_;
    partPath_ += ".part";

    auto sink = cursor::CursorNdjsonSink::create(partPath_);
    if (!sink.hasValue()) {
        fail(sink.error());
        return;
    }
    auto native = sourceFactory_(start);
    if (!native.hasValue()) {
        sink.value().reset();
        std::filesystem::remove(partPath_, error);
        fail(native.error());
        return;
    }
    auto relative = std::unique_ptr<cursor::ICursorSource>{
        new TakeRelativeCursorSource{std::move(native).value(), start.startedAt}};
    auto created = cursor::CursorEventPump::create(
        std::move(relative), std::move(sink).value(), screen->sourceId);
    if (!created.hasValue()) {
        fail(created.error());
        return;
    }
    pump_ = std::move(created).value();
    active_ = true;
    timer_.start();
    setStatus(tr("Cursor data: recording"));
}

void CursorRecordingBinding::poll() {
    if (!pump_) return;
    if (auto drained = pump_->drain(kDrainBatch); !drained.hasValue()) {
        fail(drained.error());
        return;
    }
    publishStats();
}

void CursorRecordingBinding::finish() {
    if (!pump_) return;
    timer_.stop();
    for (;;) {
        const auto before = pump_->stats().polled;
        if (auto drained = pump_->drain(kDrainBatch); !drained.hasValue()) {
            fail(drained.error());
            return;
        }
        if (pump_->stats().polled == before) break;
    }
    publishStats();
    pump_.reset();
    active_ = false;

    std::error_code error;
    if (std::filesystem::exists(finalPath_, error)) {
        fail(core::AppError{core::ErrorCode::IoFailure,
                            "cursor telemetry output already exists"});
        return;
    }
    std::filesystem::rename(partPath_, finalPath_, error);
    if (error) {
        fail(core::AppError{core::ErrorCode::IoFailure,
                            "could not publish cursor telemetry: " +
                                error.message()});
        return;
    }
    outputPath_ = fromPath(finalPath_);
    setStatus(tr("Cursor data saved (%1 events)").arg(eventCount_));
}

void CursorRecordingBinding::fail(core::AppError error) {
    timer_.stop();
    if (pump_) {
        publishStats();
        pump_.reset();
    }
    active_ = false;
    setStatus(tr("Cursor data failed; video/audio recording continues: %1")
                  .arg(fromUtf8(error.message())));
}

void CursorRecordingBinding::setStatus(QString status) {
    if (statusMessage_ == status) {
        emit stateChanged();
        return;
    }
    statusMessage_ = std::move(status);
    emit stateChanged();
}

void CursorRecordingBinding::publishStats() {
    if (!pump_) return;
    const auto& stats = pump_->stats();
    const auto next = static_cast<qulonglong>(stats.moves + stats.clicks);
    if (eventCount_ == next) return;
    eventCount_ = next;
    emit stateChanged();
}

}  // namespace creator::app
