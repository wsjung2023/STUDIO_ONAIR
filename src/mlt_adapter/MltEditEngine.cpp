#include "mlt_adapter/MltEditEngine.h"

#include "core/AppError.h"
#include "mlt_adapter/MltGraphPlan.h"
#include "mlt_adapter/MltRuntimeManifest.h"

#include <mlt++/MltFactory.h>
#include <mlt++/MltFilter.h>
#include <mlt++/MltFrame.h>
#include <mlt++/MltPlaylist.h>
#include <mlt++/MltProducer.h>
#include <mlt++/MltProfile.h>
#include <mlt++/MltRepository.h>
#include <mlt++/MltTractor.h>
#include <mlt++/MltTransition.h>
#include <framework/mlt_field.h>
#include <framework/mlt_tractor.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::mlt_adapter {
namespace {

constexpr std::uint32_t kMaximumPreviewDimension = 8192;

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

class LoadedRuntimeLibraries final {
public:
    LoadedRuntimeLibraries() = default;
    LoadedRuntimeLibraries(const LoadedRuntimeLibraries&) = delete;
    LoadedRuntimeLibraries& operator=(const LoadedRuntimeLibraries&) = delete;
    LoadedRuntimeLibraries(LoadedRuntimeLibraries&& other) noexcept
#ifdef _WIN32
        : modules_(std::move(other.modules_)),
          directoryCookie_(std::exchange(other.directoryCookie_, nullptr))
#endif
    {}
    LoadedRuntimeLibraries& operator=(LoadedRuntimeLibraries&& other) noexcept {
        if (this != &other) {
            release();
#ifdef _WIN32
            modules_ = std::move(other.modules_);
            directoryCookie_ =
                std::exchange(other.directoryCookie_, nullptr);
#endif
        }
        return *this;
    }
    ~LoadedRuntimeLibraries() { release(); }

#ifdef _WIN32
    void add(HMODULE module) { modules_.push_back(module); }
    void setDirectoryCookie(DLL_DIRECTORY_COOKIE cookie) {
        directoryCookie_ = cookie;
    }
#endif

private:
    void release() noexcept {
#ifdef _WIN32
        for (auto module = modules_.rbegin(); module != modules_.rend(); ++module) {
            if (*module) FreeLibrary(*module);
        }
        modules_.clear();
        if (directoryCookie_) {
            RemoveDllDirectory(directoryCookie_);
            directoryCookie_ = nullptr;
        }
#endif
    }

#ifdef _WIN32
    std::vector<HMODULE> modules_;
    DLL_DIRECTORY_COOKIE directoryCookie_{};
#endif
};

core::Result<LoadedRuntimeLibraries> loadRuntimeLibraries(
    const std::filesystem::path& root) {
    LoadedRuntimeLibraries loaded;
#ifdef _WIN32
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                  LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        return core::AppError{
            core::ErrorCode::UnsupportedVersion,
            "Windows could not enable the audited MLT DLL search policy (error=" +
                std::to_string(GetLastError()) + ")"};
    }
    const auto runtimeBin = root / "bin";
    const auto cookie = AddDllDirectory(runtimeBin.c_str());
    if (!cookie) {
        return core::AppError{
            core::ErrorCode::UnsupportedVersion,
            "Windows could not register the audited MLT runtime (error=" +
                std::to_string(GetLastError()) + ")"};
    }
    loaded.setDirectoryCookie(cookie);
    constexpr DWORD kSearchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                   LOAD_LIBRARY_SEARCH_USER_DIRS;
    for (const auto* name : {L"mlt-7.dll", L"mlt++-7.dll"}) {
        const auto path = root / "bin" / name;
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, kSearchFlags);
        if (!module) {
            return core::AppError{
                core::ErrorCode::UnsupportedVersion,
                "Audited MLT runtime could not be loaded by Windows (error=" +
                    std::to_string(GetLastError()) + ")"};
        }
        loaded.add(module);
    }
#else
    static_cast<void>(root);
#endif
    return loaded;
}

class MltProcessRuntime final {
public:
    static MltProcessRuntime& instance() {
        static MltProcessRuntime runtime;
        return runtime;
    }

    core::Result<void> bind(const std::filesystem::path& runtimeRoot) {
        std::lock_guard lock(mutex_);
        if (repository_) {
            std::error_code equivalentError;
            const bool sameRuntime = std::filesystem::equivalent(
                root_, runtimeRoot, equivalentError);
            if (equivalentError || !sameRuntime) {
                return stateError(
                    "MLT factory is already bound to another runtime");
            }
            return core::ok();
        }

        auto libraries = loadRuntimeLibraries(runtimeRoot);
        if (!libraries.hasValue()) return libraries.error();
        setMltEnvironment(runtimeRoot);
        const auto modules = utf8Path(runtimeRoot / "lib/mlt-7");
        Mlt::Repository* repository = Mlt::Factory::init(modules.c_str());
        if (!repository) {
            return stateError("MLT repository initialization failed");
        }
        libraries_ = std::move(libraries).value();
        repository_ = repository;
        root_ = runtimeRoot;
        return core::ok();
    }

    ~MltProcessRuntime() {
        if (repository_) {
            Mlt::Factory::close();
            // MLT allocates the C++ Repository wrapper inside its release CRT
            // DLL. Deleting it from a debug-CRT application crosses heaps on
            // Windows. It contains only the process-global native handle, so
            // the wrapper intentionally lives until process teardown while
            // Factory::close() deterministically releases the native factory.
            repository_ = nullptr;
        }
    }

private:
    MltProcessRuntime() = default;

    std::mutex mutex_;
    std::filesystem::path root_;
    LoadedRuntimeLibraries libraries_;
    Mlt::Repository* repository_{};
};

}  // namespace

class MltEditEngine::Impl final {
public:
    struct Graph final {
        std::unique_ptr<Mlt::Profile> profile;
        std::unique_ptr<Mlt::Tractor> tractor;
        std::vector<std::unique_ptr<Mlt::Playlist>> playlists;
        std::vector<std::unique_ptr<Mlt::Producer>> producers;
        std::vector<std::unique_ptr<Mlt::Filter>> filters;
        std::vector<std::unique_ptr<Mlt::Transition>> transitions;
        domain::TimelineRevision revision;
        core::FrameRate frameRate;
        std::int64_t durationFrames;
        std::size_t videoCompositeTransitions{};
        std::size_t audioMixTransitions{};
    };

    explicit Impl(MltEditEngineConfig config) : config_(std::move(config)) {}

    core::Result<void> initialize() {
        if (initialized_) return core::ok();
        auto verified = verifyMltRuntimeManifest(config_.runtimeRoot);
        if (!verified.hasValue()) return verified;
        std::error_code pathError;
        const auto runtimeRoot =
            std::filesystem::weakly_canonical(config_.runtimeRoot, pathError);
        if (pathError) {
            return stateError("MLT runtime root could not be canonicalized");
        }
        auto bound = MltProcessRuntime::instance().bind(runtimeRoot);
        if (!bound.hasValue()) return bound;
        const auto bin = utf8Path(runtimeRoot / "bin");
        const auto data = utf8Path(runtimeRoot / "share/mlt-7");
        mlt_environment_set("MLT_APPDIR", bin.c_str());
        mlt_environment_set("MLT_DATA", data.c_str());
        mlt_environment_set("MLT_PROFILES_PATH", (data + "/profiles").c_str());
        mlt_environment_set("MLT_PRESETS_PATH", (data + "/presets").c_str());
        initialized_ = true;
        return core::ok();
    }

    core::Result<std::unique_ptr<Graph>> build(const MltGraphPlan& plan) {
        if (config_.previewWidth == 0 || config_.previewHeight == 0 ||
            config_.previewWidth > kMaximumPreviewDimension ||
            config_.previewHeight > kMaximumPreviewDimension) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "MLT preview dimensions are outside the supported range"};
        }
        if (plan.durationFrames > std::numeric_limits<int>::max()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "MLT preview timeline is too long"};
        }
        const auto graphDurationFrames =
            std::max<std::int64_t>(1, plan.durationFrames);
        auto graph = std::make_unique<Graph>(Graph{
            std::make_unique<Mlt::Profile>(), nullptr, {}, {}, {}, {},
            plan.revision, plan.frameRate, graphDurationFrames, 0, 0});
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

        const auto isAudible = [](const MltGraphTrack& track) {
            return track.kind == domain::TrackKind::Audio && track.enabled &&
                   std::any_of(track.clips.begin(), track.clips.end(),
                               [](const auto& clip) {
                                   return clip.enabled && clip.available;
                               });
        };
        const MltGraphTrack* audioBase = nullptr;
        for (const auto& track : plan.tracks) {
            if (isAudible(track)) {
                audioBase = &track;
                break;
            }
        }

        int trackIndex = 0;
        std::vector<std::pair<const MltGraphTrack*, int>> nativeTracks;
        const auto appendTimelineTrack = [&](const MltGraphTrack& track)
            -> core::Result<int> {
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
                    if (!producer->is_valid()) {
                        return stateError("MLT could not open a timeline media asset");
                    }
                    if (clip.mediaKind != domain::MediaKind::Image) {
                        auto converter = std::make_unique<Mlt::Filter>(
                            *graph->profile, "audioconvert");
                        if (!converter->is_valid() ||
                            producer->attach(*converter) != 0) {
                            return stateError(
                                "MLT could not attach the audio format converter");
                        }
                        graph->filters.push_back(std::move(converter));
                    }
                    if (clip.mediaKind == domain::MediaKind::Image) {
                        if (playlist->append(*producer, 0, 0) != 0 ||
                            playlist->repeat(
                                playlist->count() - 1, length) != 0) {
                            return stateError(
                                "MLT could not hold a still image on the timeline");
                        }
                    } else if (playlist->append(
                                   *producer, static_cast<int>(clip.sourceIn),
                                   static_cast<int>(clip.sourceOut)) != 0) {
                        return stateError(
                            "MLT could not open a timeline media asset");
                    }
                    graph->producers.push_back(std::move(producer));
                }
                cursor = timelineIn + length;
            }
            if (graphDurationFrames > cursor) {
                playlist->blank(
                    static_cast<int>(graphDurationFrames) - cursor - 1);
            }
            const int nativeTrackIndex = trackIndex++;
            if (graph->tractor->set_track(*playlist, nativeTrackIndex) != 0) {
                return stateError("MLT could not connect a timeline track");
            }
            graph->playlists.push_back(std::move(playlist));
            nativeTracks.emplace_back(&track, nativeTrackIndex);
            return nativeTrackIndex;
        };

        std::optional<int> audioBaseTrackIndex;
        if (audioBase) {
            auto appended = appendTimelineTrack(*audioBase);
            if (!appended.hasValue()) return appended.error();
            audioBaseTrackIndex = appended.value();
        }

        const int backgroundTrackIndex = trackIndex++;
        auto background = std::make_unique<Mlt::Producer>(
            *graph->profile, "colour", "black");
        auto backgroundTrack =
            std::make_unique<Mlt::Playlist>(*graph->profile);
        if (!background->is_valid() ||
            backgroundTrack->append(
                *background, 0,
                static_cast<int>(graphDurationFrames) - 1) != 0 ||
            graph->tractor->set_track(*backgroundTrack,
                                      backgroundTrackIndex) != 0) {
            return stateError("MLT could not create the preview background");
        }
        graph->producers.push_back(std::move(background));
        graph->playlists.push_back(std::move(backgroundTrack));

        for (const auto& track : plan.tracks) {
            if (&track == audioBase) continue;
            auto appended = appendTimelineTrack(track);
            if (!appended.hasValue()) return appended.error();
        }

        for (const auto& [track, nativeTrackIndex] : nativeTracks) {
            if (isAudible(*track) && track != audioBase) {
                auto mix =
                    std::make_unique<Mlt::Transition>(*graph->profile, "mix");
                if (!mix->is_valid()) {
                    return stateError("MLT core audio mix transition is unavailable");
                }
                mix->set("always_active", 1);
                mix->set("sum", 1);
                if (mlt_field_plant_transition(
                        mlt_tractor_field(graph->tractor->get_tractor()),
                        mix->get_transition(), *audioBaseTrackIndex,
                        nativeTrackIndex) != 0) {
                    return stateError(
                        "MLT could not connect an audio mix transition");
                }
                ++graph->audioMixTransitions;
                graph->transitions.push_back(std::move(mix));
            }
        }
        for (const auto& [track, nativeTrackIndex] : nativeTracks) {
            if (track->kind == domain::TrackKind::Video) {
                auto composite = std::make_unique<Mlt::Transition>(
                    *graph->profile, "composite");
                if (!composite->is_valid()) {
                    return stateError("MLT core composite transition is unavailable");
                }
                composite->set("always_active", 1);
                composite->set("progressive", 1);
                composite->set("distort", 1);
                if (mlt_field_plant_transition(
                        mlt_tractor_field(graph->tractor->get_tractor()),
                        composite->get_transition(), backgroundTrackIndex,
                        nativeTrackIndex) != 0) {
                    return stateError(
                        "MLT could not connect a video composite transition");
                }
                ++graph->videoCompositeTransitions;
                graph->transitions.push_back(std::move(composite));
            }
        }
        graph->tractor->refresh();
        return graph;
    }

    MltEditEngineConfig config_;
    std::unique_ptr<Graph> graph_;
    core::TimestampNs position_{};
    bool playing_{false};
    bool initialized_{false};
};

MltEditEngine::MltEditEngine(MltEditEngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
MltEditEngine::~MltEditEngine() = default;

core::Result<void> MltEditEngine::preflightRuntime(
    const std::filesystem::path& runtimeRoot) {
    auto verified = verifyMltRuntimeManifest(runtimeRoot);
    if (!verified.hasValue()) return verified;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    if (error) return stateError("MLT runtime root could not be canonicalized");
    auto libraries = loadRuntimeLibraries(root);
    if (!libraries.hasValue()) return libraries.error();
    return core::ok();
}

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
    mlt_image_format format = mlt_image_rgba;
    std::uint8_t* image = nullptr;
    const int imageResult = mlt_frame_get_image(
        frame->get_frame(), &image, &format, &width, &height, 0);
    if (imageResult != 0 || !image || width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaximumPreviewDimension) ||
        height > static_cast<int>(kMaximumPreviewDimension)) {
        return stateError(
            "MLT could not decode a preview frame (result=" +
            std::to_string(imageResult) + ", format=" +
            std::string{mlt_image_format_name(format)} + ")");
    }
    const auto widthSize = static_cast<std::size_t>(width);
    const auto heightSize = static_cast<std::size_t>(height);
    if (heightSize > std::numeric_limits<std::size_t>::max() / widthSize) {
        return stateError("MLT preview dimensions overflow the pixel count");
    }
    const auto pixelCount = widthSize * heightSize;
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U) {
        return stateError("MLT preview dimensions overflow the byte count");
    }
    const auto byteCount = pixelCount * 4U;
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(byteCount);
    if (format == mlt_image_rgba) {
        for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
            (*pixels)[offset] = image[offset + 2U];
            (*pixels)[offset + 1U] = image[offset + 1U];
            (*pixels)[offset + 2U] = image[offset];
            (*pixels)[offset + 3U] = image[offset + 3U];
        }
    } else if (format == mlt_image_yuv422 && width % 2 == 0) {
        int alphaSize = 0;
        const auto* alpha =
            mlt_frame_get_alpha_size(frame->get_frame(), &alphaSize);
        if (alphaSize < 0 ||
            (alpha && alphaSize > 0 &&
             static_cast<std::size_t>(alphaSize) < pixelCount)) {
            return stateError("MLT returned an invalid preview alpha plane");
        }
        const bool hasSizedAlpha =
            alpha && alphaSize > 0 &&
            static_cast<std::size_t>(alphaSize) >= pixelCount;
        const auto clamp = [](int value) {
            return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
        };
        for (std::size_t pixel = 0; pixel < pixelCount; pixel += 2U) {
            const std::size_t source = pixel * 2U;
            const int u = static_cast<int>(image[source + 1U]) - 128;
            const int v = static_cast<int>(image[source + 3U]) - 128;
            for (std::size_t withinPair = 0; withinPair < 2U; ++withinPair) {
                // The graph profile is explicitly Rec.709. MLT's core
                // compositor returns limited-range YUV422 in that profile.
                const int y = static_cast<int>(
                                  image[source + withinPair * 2U]) -
                              16;
                const int scaledY = 298 * std::max(y, 0);
                const int red = (scaledY + 459 * v + 128) >> 8;
                const int green =
                    (scaledY - 55 * u - 136 * v + 128) >> 8;
                const int blue = (scaledY + 541 * u + 128) >> 8;
                const std::size_t destination = (pixel + withinPair) * 4U;
                (*pixels)[destination] = clamp(blue);
                (*pixels)[destination + 1U] = clamp(green);
                (*pixels)[destination + 2U] = clamp(red);
                (*pixels)[destination + 3U] =
                    hasSizedAlpha ? alpha[pixel + withinPair] : 255U;
            }
        }
    } else {
        return stateError("MLT returned an unsupported preview pixel format");
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

core::Result<MltEditEngineDiagnostics> MltEditEngine::diagnostics() const {
    if (!impl_->graph_) return stateError("MLT timeline is not loaded");
    return MltEditEngineDiagnostics{
        .nativeTrackCount =
            static_cast<std::size_t>(impl_->graph_->tractor->count()),
        .videoCompositeTransitions =
            impl_->graph_->videoCompositeTransitions,
        .audioMixTransitions = impl_->graph_->audioMixTransitions};
}

core::Result<std::vector<float>> MltEditEngine::requestMixedAudio(
    core::TimestampNs position, int frequency, int channels, int samples) {
    if (!impl_->graph_) return stateError("MLT timeline is not loaded");
    if (frequency <= 0 || frequency > 384'000 || channels <= 0 || channels > 32 ||
        samples <= 0 || samples > frequency * 2) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "MLT audio request is outside supported limits"};
    }
    const auto frameNumber =
        core::timestampToFrame(position, impl_->graph_->frameRate);
    if (frameNumber < 0 || frameNumber >= impl_->graph_->durationFrames ||
        frameNumber > std::numeric_limits<int>::max()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "audio position is outside the timeline"};
    }
    impl_->graph_->tractor->seek(static_cast<int>(frameNumber));
    std::unique_ptr<Mlt::Frame> frame{impl_->graph_->tractor->get_frame()};
    if (!frame || !frame->is_valid()) {
        return stateError("MLT did not return an audio frame");
    }
    void* buffer = nullptr;
    mlt_audio_format format = mlt_audio_f32le;
    int returnedFrequency = frequency;
    int returnedChannels = channels;
    int returnedSamples = samples;
    const int result = mlt_frame_get_audio(
        frame->get_frame(), &buffer, &format, &returnedFrequency,
        &returnedChannels, &returnedSamples);
    if (result != 0 || !buffer ||
        returnedFrequency != frequency || returnedChannels != channels ||
        returnedSamples <= 0 || returnedSamples > samples) {
        return stateError(
            "MLT could not produce the requested mixed audio (result=" +
            std::to_string(result) + ", format=" +
            std::string{mlt_audio_format_name(format)} + ", frequency=" +
            std::to_string(returnedFrequency) + ", channels=" +
            std::to_string(returnedChannels) + ", samples=" +
            std::to_string(returnedSamples) + ")");
    }
    const auto sampleCount = static_cast<std::size_t>(returnedSamples) *
                             static_cast<std::size_t>(returnedChannels);
    if (format == mlt_audio_f32le) {
        const auto* values = static_cast<const float*>(buffer);
        return std::vector<float>{values, values + sampleCount};
    }
    if (format == mlt_audio_s16) {
        const auto* values = static_cast<const std::int16_t*>(buffer);
        std::vector<float> converted(sampleCount);
        std::transform(values, values + sampleCount, converted.begin(),
                       [](std::int16_t value) {
                           return static_cast<float>(value) / 32768.0F;
                       });
        return converted;
    }
    return stateError("MLT returned an unsupported mixed audio format");
}

core::Result<std::unique_ptr<edit_engine::IRenderJob>> MltEditEngine::render(
    const edit_engine::RenderRequest&) {
    return core::AppError{core::ErrorCode::InvalidState,
                          "MLT render jobs are implemented in R1-05"};
}

}  // namespace creator::mlt_adapter
