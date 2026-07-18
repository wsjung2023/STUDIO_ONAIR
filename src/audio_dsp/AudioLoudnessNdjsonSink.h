#pragma once

#include "audio_dsp/AudioLoudnessSample.h"
#include "core/Result.h"

#include <filesystem>
#include <fstream>

namespace creator::audio_dsp {

/// Append-only NDJSON writer for `audio.loudness` telemetry.
///
/// Responsibility: record a growing stream of loudness readings, one serialized
/// JSON object per line (newline-delimited JSON), in the order they are
/// appended. Telemetry is inherently sequential and unbounded, so — unlike a
/// whole-document manifest — a full-file atomic rename does not fit: instead
/// each `append` writes one complete line and flushes it to the OS before
/// returning. The stream is opened in append mode (writes are ordered and never
/// clobber existing content) and closed by the destructor (RAII); the file is
/// created if absent.
///
/// Durability scope: `flush()` hands the bytes to the operating system, so a
/// *process* crash loses only trailing lines that were never appended, never a
/// partial/corrupt line. It does NOT `fsync`, so an OS crash or power loss can
/// still drop already-flushed tail lines still buffered by the OS. If
/// power-loss durability is ever required, an explicit fsync option could be
/// added here.
///
/// Boundary guards: every sample goes through AudioLoudnessSerializer, which
/// rejects a negative timestamp and maps non-finite measurements to JSON null,
/// so an invalid line is never written. The sink is not internally synchronised;
/// a single writer thread is assumed.
class AudioLoudnessNdjsonSink final {
public:
    /// Open (creating if necessary) `target` for append. Fails with
    /// ErrorCode::IoFailure if the stream cannot be opened.
    [[nodiscard]] static core::Result<AudioLoudnessNdjsonSink> open(
        const std::filesystem::path& target);

    AudioLoudnessNdjsonSink(AudioLoudnessNdjsonSink&&) noexcept = default;
    AudioLoudnessNdjsonSink& operator=(AudioLoudnessNdjsonSink&&) noexcept =
        default;
    AudioLoudnessNdjsonSink(const AudioLoudnessNdjsonSink&) = delete;
    AudioLoudnessNdjsonSink& operator=(const AudioLoudnessNdjsonSink&) = delete;
    ~AudioLoudnessNdjsonSink() = default;

    /// Serialize `sample` and append it as one flushed NDJSON line. Fails with
    /// ErrorCode::InvalidArgument if the sample cannot be serialized (e.g.
    /// negative tNs) — in which case nothing is written — or with
    /// ErrorCode::IoFailure if the write/flush fails.
    [[nodiscard]] core::Result<void> append(const AudioLoudnessSample& sample);

    /// Path this sink writes to.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    AudioLoudnessNdjsonSink(std::filesystem::path path, std::ofstream stream)
        : path_(std::move(path)), stream_(std::move(stream)) {}

    std::filesystem::path path_;
    std::ofstream stream_;
};

}  // namespace creator::audio_dsp
