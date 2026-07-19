#include "edit_engine/UnavailableEditEngine.h"

#include "core/AppError.h"

namespace creator::edit_engine {
namespace {

core::AppError unavailable() {
    return {core::ErrorCode::InvalidState,
            "Editor playback engine is unavailable in this build"};
}

}  // namespace

core::Result<void> UnavailableEditEngine::load(const TimelineSnapshot&) {
    return unavailable();
}

core::Result<void> UnavailableEditEngine::update(const TimelineChangeSet&) {
    return unavailable();
}

core::Result<void> UnavailableEditEngine::play() { return unavailable(); }

core::Result<void> UnavailableEditEngine::pause() { return unavailable(); }

core::Result<void> UnavailableEditEngine::seek(core::TimestampNs) {
    return unavailable();
}

core::Result<PreviewFrame> UnavailableEditEngine::requestFrame(
    core::TimestampNs) {
    return unavailable();
}

core::Result<std::unique_ptr<IRenderJob>> UnavailableEditEngine::render(
    const RenderRequest&) {
    return unavailable();
}

}  // namespace creator::edit_engine
