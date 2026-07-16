#include "mlt_adapter/MltEditEngine.h"

#include "core/AppError.h"
#include "mlt_adapter/MltGraphPlan.h"
#include "mlt_adapter/MltRuntimeManifest.h"

#include <mlt++/MltFactory.h>
#include <mlt++/MltFrame.h>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>
#include <mlt++/MltRepository.h>
#include <mlt++/MltTractor.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::mlt_adapter {
namespace {

core::AppError stateError(std::string message) {
    return core::AppError{core::ErrorCode::InvalidState, std::move(message)};
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

void setMltEnvironment(const std::filesystem::path& root) {
    const auto bin = utf8Path(root / "bin");
    const auto data = utf8Path(root / "share/mlt-7");
#ifdef _WIN32
    using PutEnvFunction = int(__cdecl*)(const char*, const char*);
    const auto runtime = GetModuleHandleW(L"ucrtbase.dll");
    const auto putEnvironment = runtime
        ? reinterpret_cast<PutEnvFunction>(GetProcAddress(runtime, "_putenv_s"))
        : nullptr;
    const auto setEnvironment = [putEnvironment](const char* name,
                                                  const std::string& value) {
        SetEnvironmentVariableA(name, value.c_str());
        if (putEnvironment) putEnvironment(name, value.c_str());
    };
    setEnvironment("MLT_APPDIR", bin);
    setEnvironment("MLT_DATA", data);
    setEnvironment("MLT_PROFILES_PATH", data + "/profiles");
    setEnvironment("MLT_PRESETS_PATH", data + "/presets");
#else
    setenv("MLT_APPDIR", bin.c_str(), 1);
    setenv("MLT_DATA", data.c_str(), 1);
    setenv("MLT_PROFILES_PATH", (data + "/profiles").c_str(), 1);
    setenv("MLT_PRESETS_PATH", (data + "/presets").c_str(), 1);
#endif
}

}  // namespace

class MltEditEngine::Impl final {
public:
    struct Graph final {
        std::unique_ptr<Mlt::Profile> profile;
        std::unique_ptr<Mlt::Tractor> tractor;
        std::vector<std::unique_ptr<Mlt::Playlist>> playlists;
        std::vector<std::unique_ptr<Mlt::Producer>> producers;
        domain::TimelineRevision revision;
        core::FrameRate frameRate;
        std::int64_t durationFrames;
    };

    explicit Impl(MltEditEngineConfig config) : config_(std::move(config)) {}

    core::Result<void> initialize() {
        auto verified = verifyMltRuntimeManifest(config_.runtimeRoot);
        if (!verified.hasValue()) return verified;
        setMltEnvironment(config_.runtimeRoot);
        static std::mutex factoryMutex;
        std::lock_guard lock(factoryMutex);
        static Mlt::Repository* repository = nullptr;
        if (!repository) {
            const auto modules = utf8Path(config_.runtimeRoot / "lib/mlt-7");
            repository = Mlt::Factory::init(modules.c_str());
        }
        if (!repository) return stateError("MLT repository initialization failed");
        const auto bin = utf8Path(config_.runtimeRoot / "bin");
        const auto data = utf8Path(config_.runtimeRoot / "share/mlt-7");
        mlt_environment_set("MLT_APPDIR", bin.c_str());
        mlt_environment_set("MLT_DATA", data.c_str());
        mlt_environment_set("MLT_PROFILES_PATH", (data + "/profiles").c_str());
        mlt_environment_set("MLT_PRESETS_PATH", (data + "/presets").c_str());
        return core::ok();
    }

    core::Result<std::unique_ptr<Graph>> build(const MltGraphPlan& plan) {
        if (plan.durationFrames > std::numeric_limits<int>::max()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "MLT preview timeline is too long"};
        }
        auto graph = std::make_unique<Graph>(Graph{
            std::make_unique<Mlt::Profile>(), nullptr, {}, {}, plan.revision,
            plan.frameRate, plan.durationFrames});
        graph->profile->set_explicit(1);
        graph->profile->set_width(static_cast<int>(config_.previewWidth));
        graph->profile->set_height(static_cast<int>(config_.previewHeight));
        graph->profile->set_progressive(1);
        graph->profile->set_colorspace(709);
        graph->profile->set_sample_aspect(1, 1);
        graph->profile->set_display_aspect(
            static_cast<int>(config_.previewWidth),
            static_cast<int>(config_.previewHeight));
        graph->profile->set_frame_rate(
            static_cast<int>(plan.frameRate.numerator()),
            static_cast<int>(plan.frameRate.denominator()));
        graph->tractor = std::make_unique<Mlt::Tractor>(*graph->profile);

        int trackIndex = 0;
        for (const auto& track : plan.tracks) {
            auto playlist = std::make_unique<Mlt::Playlist>(*graph->profile);
            int cursor = 0;
            for (const auto& clip : track.clips) {
                const int timelineIn = static_cast<int>(clip.timelineIn);
                const int length = static_cast<int>(clip.timelineOut - clip.timelineIn + 1);
                if (timelineIn > cursor) playlist->blank(timelineIn - cursor - 1);
                if (!track.enabled || !clip.enabled || !clip.available) {
                    playlist->blank(length - 1);
                } else {
                    auto producer = std::make_unique<Mlt::Producer>(
                        *graph->profile, "avformat", utf8Path(clip.mediaPath).c_str());
                    if (!producer->is_valid() ||
                        playlist->append(*producer, static_cast<int>(clip.sourceIn),
                                         static_cast<int>(clip.sourceOut)) != 0) {
                        return stateError("MLT could not open a timeline media asset");
                    }
                    graph->producers.push_back(std::move(producer));
                }
                cursor = timelineIn + length;
            }
            if (plan.durationFrames > cursor) {
                playlist->blank(static_cast<int>(plan.durationFrames) - cursor - 1);
            }
            if (graph->tractor->set_track(*playlist, trackIndex++) != 0) {
                return stateError("MLT could not connect a timeline track");
            }
            graph->playlists.push_back(std::move(playlist));
        }
        graph->tractor->refresh();
        return graph;
    }

    MltEditEngineConfig config_;
    std::unique_ptr<Graph> graph_;
    core::TimestampNs position_{};
    bool playing_{false};
};

MltEditEngine::MltEditEngine(MltEditEngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
MltEditEngine::~MltEditEngine() = default;

core::Result<void> MltEditEngine::load(
    const edit_engine::TimelineSnapshot& snapshot) {
    auto initialized = impl_->initialize();
    if (!initialized.hasValue()) return initialized;
    auto plan = compileMltGraphPlan(snapshot);
    if (!plan.hasValue()) return plan.error();
    auto graph = impl_->build(plan.value());
    if (!graph.hasValue()) return graph.error();
    impl_->graph_ = std::move(graph).value();
    impl_->position_ = core::TimestampNs{};
    impl_->playing_ = false;
    return core::ok();
}

core::Result<void> MltEditEngine::update(
    const edit_engine::TimelineChangeSet& change) {
    if (!impl_->graph_ || change.baseRevision() != impl_->graph_->revision) {
        return stateError("MLT update does not match the loaded revision");
    }
    return load(change.target());
}

core::Result<void> MltEditEngine::play() {
    if (!impl_->graph_) return stateError("MLT timeline is not loaded");
    impl_->playing_ = true;
    return core::ok();
}

core::Result<void> MltEditEngine::pause() {
    if (!impl_->graph_) return stateError("MLT timeline is not loaded");
    impl_->playing_ = false;
    return core::ok();
}

core::Result<void> MltEditEngine::seek(core::TimestampNs position) {
    if (!impl_->graph_ || position.time_since_epoch().count() < 0) {
        return stateError("MLT seek requires a loaded timeline and valid position");
    }
    impl_->position_ = position;
    return core::ok();
}

core::Result<edit_engine::PreviewFrame> MltEditEngine::requestFrame(
    core::TimestampNs position) {
    if (!impl_->graph_) return stateError("MLT timeline is not loaded");
    const auto frameNumber =
        core::timestampToFrame(position, impl_->graph_->frameRate);
    if (frameNumber < 0 || frameNumber >= impl_->graph_->durationFrames ||
        frameNumber > std::numeric_limits<int>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "preview position is outside the timeline"};
    }
    impl_->graph_->tractor->seek(static_cast<int>(frameNumber));
    std::unique_ptr<Mlt::Frame> frame{impl_->graph_->tractor->get_frame()};
    if (!frame || !frame->is_valid()) return stateError("MLT did not return a frame");
    int width = static_cast<int>(impl_->config_.previewWidth);
    int height = static_cast<int>(impl_->config_.previewHeight);
    auto* image = frame->fetch_image(mlt_image_rgba, width, height, 0);
    if (!image || width <= 0 || height <= 0) {
        return stateError("MLT could not decode a BGRA preview frame");
    }
    const auto byteCount = static_cast<std::size_t>(width) *
                           static_cast<std::size_t>(height) * 4U;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(image,
                                                               image + byteCount);
    for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
        std::swap((*pixels)[offset], (*pixels)[offset + 2U]);
    }
    media::VideoFrame video{
        .timestamp = position,
        .width = static_cast<std::uint32_t>(width),
        .height = static_cast<std::uint32_t>(height),
        .visibleRect = {.x = 0, .y = 0,
                        .width = static_cast<std::uint32_t>(width),
                        .height = static_cast<std::uint32_t>(height)},
        .contentWidth = static_cast<std::uint32_t>(width),
        .contentHeight = static_cast<std::uint32_t>(height),
        .contentScale = 1.0,
        .pointPixelScale = 1.0,
        .pixelFormat = media::PixelFormat::Bgra8,
        .colorSpace = media::ColorSpace::Rec709Sdr,
        .platformHandle = std::shared_ptr<void>{pixels, pixels->data()}};
    impl_->position_ = position;
    return edit_engine::PreviewFrame::create(position, impl_->graph_->revision,
                                             std::move(video));
}

core::Result<std::unique_ptr<edit_engine::IRenderJob>> MltEditEngine::render(
    const edit_engine::RenderRequest&) {
    return core::AppError{core::ErrorCode::InvalidState,
                          "MLT render jobs are implemented in R1-05"};
}

}  // namespace creator::mlt_adapter
