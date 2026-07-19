#include "transcription/UnavailableTranscriptionProvider.h"

namespace creator::transcription {

namespace {

core::AppError notBuilt() {
    // UnsupportedVersion is the same category edit_engine / live-recording use for
    // "this capability was not compiled into the running build".
    return core::AppError{
        core::ErrorCode::UnsupportedVersion,
        "whisper.cpp transcription is not available in this build "
        "(built without CS_ENABLE_WHISPER)"};
}

}  // namespace

UnavailableTranscriptionProvider::UnavailableTranscriptionProvider()
    : error_(notBuilt()) {}

UnavailableTranscriptionProvider::UnavailableTranscriptionProvider(
    core::AppError error)
    : error_(std::move(error)) {}

core::Result<Transcript> UnavailableTranscriptionProvider::transcribe(
    const AudioInput&, const TranscriptionOptions&) {
    return error_;
}

}  // namespace creator::transcription
