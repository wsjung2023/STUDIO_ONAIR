#include "mlt_adapter/MltEditEngine.h"

#include "core/AppError.h"
#include "core/Uuid.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "mlt_adapter/ExportEncoderProbe.h"
#include "mlt_adapter/FrameEffects.h"
#include "mlt_adapter/MltGraphPlan.h"
#include "mlt_adapter/MltRenderJob.h"
#include "mlt_adapter/MltRuntimeManifest.h"

#include <mlt++/MltConsumer.h>
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
#include <framework/mlt_filter.h>
#include <framework/mlt_frame.h>
#include <framework/mlt_pool.h>
#include <framework/mlt_properties.h>
#include <framework/mlt_tractor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
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

core::AppError ioError(std::string message) {
    return core::AppError{core::ErrorCode::IoFailure, std::move(message)};
}

class PartialArtifact final {
public:
    explicit PartialArtifact(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~PartialArtifact() {
        if (!published_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }
    void published() noexcept { published_ = true; }

private:
    std::filesystem::path path_;
    bool published_{};
};

core::Result<void> publishAtomically(
    const std::filesystem::path& partial,
    const std::filesystem::path& destination,
    edit_engine::RenderOverwritePolicy overwritePolicy) {
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwritePolicy == edit_engine::RenderOverwritePolicy::ReplaceExisting) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(partial.c_str(), destination.c_str(), flags)) {
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "export destination already exists"};
        }
        return ioError("export artifact could not be atomically published");
    }
#else
    std::error_code error;
    if (overwritePolicy == edit_engine::RenderOverwritePolicy::FailIfExists &&
        std::filesystem::exists(destination, error)) {
        return core::AppError{core::ErrorCode::AlreadyExists,
                              "export destination already exists"};
    }
    std::filesystem::rename(partial, destination, error);
    if (error) return ioError("export artifact could not be atomically published");
#endif
    return core::ok();
}

std::string utf8Path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

bool requiresPackedAlphaExtraction(const MltVisualBranch& branch) {
    if (branch.sourceKind == MltVisualSourceKind::Generated) return true;
    if (branch.mediaKind != domain::MediaKind::Image) return false;
    auto extension = branch.sourcePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return extension == ".png";
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
    setEnvironment("MLT_AVFORMAT_THREADS", "1");
    setEnvironment("MLT_AVFORMAT_PRODUCER_CACHE", "4");
#else
    setenv("MLT_APPDIR", bin.c_str(), 1);
    setenv("MLT_DATA", data.c_str(), 1);
    setenv("MLT_PROFILES_PATH", (data + "/profiles").c_str(), 1);
    setenv("MLT_PRESETS_PATH", (data + "/presets").c_str(), 1);
    setenv("MLT_AVFORMAT_THREADS", "1", 1);
    setenv("MLT_AVFORMAT_PRODUCER_CACHE", "4", 1);
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

struct CreatorFilterContext {
    virtual ~CreatorFilterContext() = default;
};

struct VisualFilterContext final : CreatorFilterContext {
    VisualFilterContext(domain::VisualTransform value,
                        std::uint32_t width, std::uint32_t height)
        : transform(value), canvasWidth(width), canvasHeight(height) {}
    domain::VisualTransform transform;
    std::uint32_t canvasWidth;
    std::uint32_t canvasHeight;
    std::atomic<int> lastErrorStage{};
};

struct AudioFilterContext final : CreatorFilterContext {
    AudioFilterContext(domain::AudioEnvelope value, core::FrameRate rate,
                       std::uint64_t frameCount)
        : envelope(value), frameRate(rate), clipFrameCount(frameCount) {}
    domain::AudioEnvelope envelope;
    core::FrameRate frameRate;
    std::uint64_t clipFrameCount;
    std::atomic<int> lastErrorStage{};
};

void closeCreatorFilter(mlt_filter filter) {
    delete static_cast<CreatorFilterContext*>(filter->child);
    filter->child = nullptr;
    filter->close = nullptr;
    filter->parent.close = nullptr;
    mlt_service_close(&filter->parent);
}

mlt_frame processVisualFrame(mlt_filter filter, mlt_frame frame);
mlt_frame processAudioFrame(mlt_filter filter, mlt_frame frame);

core::Result<std::unique_ptr<Mlt::Filter>> makeCreatorFilter(
    std::unique_ptr<CreatorFilterContext> context,
    mlt_frame (*process)(mlt_filter, mlt_frame)) {
    mlt_filter raw = mlt_filter_new();
    if (!raw) {
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "MLT could not allocate a Creator filter"};
    }
    raw->child = context.get();
    raw->process = process;
    raw->close = closeCreatorFilter;
    std::unique_ptr<Mlt::Filter> wrapper;
    try {
        wrapper = std::make_unique<Mlt::Filter>(raw);
    } catch (const std::bad_alloc&) {
        raw->child = nullptr;
        mlt_filter_close(raw);
        return core::AppError{core::ErrorCode::InsufficientStorage,
                              "MLT Creator filter wrapper allocation failed"};
    }
    context.release();
    mlt_filter_close(raw);
    return wrapper;
}

int getTransformedImage(mlt_frame frame, std::uint8_t** image,
                        mlt_image_format* format, int* width, int* height,
                        int) noexcept {
    auto* filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    auto* context =
        filter ? static_cast<VisualFilterContext*>(filter->child) : nullptr;
    if (!context || !image || !format || !width || !height)
        return 1;
    const auto fail = [context](int stage) {
        context->lastErrorStage.store(stage, std::memory_order_relaxed);
        return 1;
    };
    try {
        *format = mlt_image_rgba;
        const int result =
            mlt_frame_get_image(frame, image, format, width, height, 1);
        if (result != 0 || !*image || *width <= 0 || *height <= 0 ||
            *width > static_cast<int>(kMaximumPreviewDimension) ||
            *height > static_cast<int>(kMaximumPreviewDimension)) {
            return fail(2);
        }
        const auto pixelCount = static_cast<std::uint64_t>(*width) *
                                static_cast<std::uint64_t>(*height);
        if (pixelCount > static_cast<std::uint64_t>(
                             std::numeric_limits<std::size_t>::max() / 4U)) {
            return fail(3);
        }
        const auto byteCount = static_cast<std::size_t>(pixelCount * 4U);
        std::vector<std::uint8_t> premultiplied;
        try {
            premultiplied.assign(*image, *image + byteCount);
        } catch (const std::bad_alloc&) {
            return fail(4);
        }
        for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
            const auto alpha = premultiplied[offset + 3U];
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                premultiplied[offset + channel] = static_cast<std::uint8_t>(
                    (static_cast<unsigned>(premultiplied[offset + channel]) *
                         alpha +
                     127U) /
                    255U);
            }
        }
        auto transformed = applyVisualTransform(
            BgraFrameView{premultiplied, static_cast<std::uint32_t>(*width),
                          static_cast<std::uint32_t>(*height),
                          static_cast<std::uint32_t>(*width) * 4U},
            context->canvasWidth, context->canvasHeight, context->transform);
        if (!transformed.hasValue() ||
            transformed.value().bytes().size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return fail(5);
        }
        const auto outputBytes = transformed.value().bytes();
        const auto outputPixels = outputBytes.size() / 4U;
        if (outputPixels >
            static_cast<std::size_t>(std::numeric_limits<int>::max() / 3)) {
            return fail(6);
        }
        const auto rgbBytes = outputPixels * 3U;
        auto* rgb = static_cast<std::uint8_t*>(
            mlt_pool_alloc(static_cast<int>(rgbBytes)));
        auto* alphaPlane = static_cast<std::uint8_t*>(
            mlt_pool_alloc(static_cast<int>(outputPixels)));
        if (!rgb || !alphaPlane) {
            if (rgb)
                mlt_pool_release(rgb);
            if (alphaPlane)
                mlt_pool_release(alphaPlane);
            return fail(6);
        }
        for (std::size_t pixel = 0; pixel < outputPixels; ++pixel) {
            const auto source = pixel * 4U;
            const auto destination = pixel * 3U;
            const auto alpha = outputBytes[source + 3U];
            alphaPlane[pixel] = alpha;
            for (std::size_t channel = 0; channel < 3U; ++channel) {
                rgb[destination + channel] =
                    alpha == 0U
                        ? 0U
                        : static_cast<std::uint8_t>(std::min(
                              255U, (static_cast<unsigned>(
                                         outputBytes[source + channel]) *
                                         255U +
                                     alpha / 2U) /
                                        alpha));
            }
        }
        if (mlt_frame_set_image(frame, rgb, static_cast<int>(rgbBytes),
                                mlt_pool_release) != 0) {
            mlt_pool_release(rgb);
            mlt_pool_release(alphaPlane);
            return fail(7);
        }
        if (mlt_frame_set_alpha(frame, alphaPlane,
                                static_cast<int>(outputPixels),
                                mlt_pool_release) != 0) {
            mlt_pool_release(alphaPlane);
            return fail(8);
        }
        *image = rgb;
        *format = mlt_image_rgb;
        *width = static_cast<int>(context->canvasWidth);
        *height = static_cast<int>(context->canvasHeight);
        return 0;
    } catch (...) {
        return fail(9);
    }
}

mlt_frame processVisualFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getTransformedImage);
    return frame;
}

std::optional<std::uint64_t>
samplesAtTimelineFrame(std::uint64_t frame, std::uint32_t frequency,
                       core::FrameRate rate) noexcept {
    const auto numerator = static_cast<std::uint64_t>(rate.numerator());
    const auto denominator = static_cast<std::uint64_t>(rate.denominator());
    if (numerator == 0U || denominator == 0U || frequency == 0U ||
        denominator > std::numeric_limits<std::uint64_t>::max() / frequency) {
        return std::nullopt;
    }
    const auto samplesPerNumerator = denominator * frequency;
    const auto quotient = frame / numerator;
    const auto remainder = frame % numerator;
    if (quotient > std::numeric_limits<std::uint64_t>::max() /
                       samplesPerNumerator ||
        remainder > std::numeric_limits<std::uint64_t>::max() /
                        samplesPerNumerator) {
        return std::nullopt;
    }
    const auto whole = quotient * samplesPerNumerator;
    const auto partial = (remainder * samplesPerNumerator) / numerator;
    if (whole > std::numeric_limits<std::uint64_t>::max() - partial) {
        return std::nullopt;
    }
    return whole + partial;
}

int getEnvelopedAudio(mlt_frame frame, void** buffer, mlt_audio_format* format,
                      int* frequency, int* channels, int* samples) noexcept {
    auto* filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    auto* context =
        filter ? static_cast<AudioFilterContext*>(filter->child) : nullptr;
    if (!context || !buffer || !format || !frequency || !channels || !samples) {
        return 1;
    }
    const auto fail = [context](int stage) {
        context->lastErrorStage.store(stage, std::memory_order_relaxed);
        return 1;
    };
    try {
        *format = mlt_audio_f32le;
        const int result = mlt_frame_get_audio(frame, buffer, format, frequency,
                                               channels, samples);
        if (result != 0 || !*buffer || *format != mlt_audio_f32le ||
            *frequency <= 0 || *channels <= 0 || *samples < 0) {
            return fail(2);
        }
        const auto clipPosition = mlt_properties_get_position(
            MLT_FRAME_PROPERTIES(frame), "meta.playlist.clip_position");
        if (clipPosition < 0) {
            return fail(3);
        }
        const auto firstSample = samplesAtTimelineFrame(
            static_cast<std::uint64_t>(clipPosition),
            static_cast<std::uint32_t>(*frequency), context->frameRate);
        const auto totalSamples = samplesAtTimelineFrame(
            context->clipFrameCount, static_cast<std::uint32_t>(*frequency),
            context->frameRate);
        if (!firstSample.has_value() || !totalSamples.has_value() ||
            *totalSamples == 0U) {
            return fail(4);
        }
        const auto sampleValues = static_cast<std::size_t>(*samples) *
                                  static_cast<std::size_t>(*channels);
        auto applied = applyAudioEnvelope(
            std::span<float>{static_cast<float*>(*buffer), sampleValues},
            static_cast<std::uint32_t>(*channels), *firstSample, *totalSamples,
            static_cast<std::uint32_t>(*frequency), context->envelope);
        return applied.hasValue() ? 0 : fail(5);
    } catch (...) {
        return fail(6);
    }
}

mlt_frame processAudioFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getEnvelopedAudio));
    return frame;
}

bool isIdentityTransform(const domain::VisualTransform& transform) noexcept {
    return transform.x() == 0.0 && transform.y() == 0.0 &&
           transform.width() == 1.0 && transform.height() == 1.0 &&
           transform.scaleX() == 1.0 && transform.scaleY() == 1.0 &&
           transform.rotationDegrees() == 0.0 && transform.cropLeft() == 0.0 &&
           transform.cropTop() == 0.0 && transform.cropRight() == 0.0 &&
           transform.cropBottom() == 0.0 && transform.opacity() == 1.0;
}

} // namespace

class MltEditEngine::Impl final {
  public:
    struct Graph final {
        std::unique_ptr<Mlt::Profile> profile;
        std::unique_ptr<Mlt::Tractor> tractor;
        std::vector<std::unique_ptr<Mlt::Playlist>> playlists;
        std::vector<std::unique_ptr<Mlt::Producer>> producers;
        std::vector<std::unique_ptr<Mlt::Filter>> filters;
        std::vector<std::unique_ptr<Mlt::Transition>> transitions;
        std::vector<VisualFilterContext*> visualFilterContexts;
        std::vector<AudioFilterContext*> audioFilterContexts;
        domain::TimelineRevision revision;
        core::FrameRate frameRate;
        std::int64_t durationFrames;
        std::size_t videoCompositeTransitions{};
        std::size_t audioMixTransitions{};
        std::size_t visualBranchCount{};
        std::size_t transformedVisualBranchCount{};
        std::size_t audioEnvelopeBranchCount{};
        std::size_t missingOverlayCount{};
    };

    explicit Impl(MltEditEngineConfig config) : config_(std::move(config)) {}

    core::Result<void> initialize() {
        if (initialized_) return core::ok();
        auto bound = MltEditEngine::initializeRuntime(config_.runtimeRoot);
        if (!bound.hasValue()) return bound;
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
        if (plan.frameRate.numerator() > std::numeric_limits<int>::max() ||
            plan.frameRate.denominator() > std::numeric_limits<int>::max()) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "timeline frame rate exceeds the MLT integer range"};
        }
        const auto sourceFitsMlt = [](std::int64_t first,
                                      std::int64_t last) noexcept {
            return first >= 0 && last >= first &&
                   last <= std::numeric_limits<int>::max();
        };
        for (const auto& track : plan.audioTracks) {
            for (const auto& clip : track.clips) {
                if (!sourceFitsMlt(clip.sourceIn, clip.sourceOut)) {
                    return core::AppError{
                        core::ErrorCode::InvalidArgument,
                        "audio source position exceeds the MLT frame range"};
                }
            }
        }
        for (const auto& branch : plan.visualBranches) {
            if (!sourceFitsMlt(branch.sourceIn, branch.sourceOut)) {
                return core::AppError{
                    core::ErrorCode::InvalidArgument,
                    "visual source position exceeds the MLT frame range"};
            }
        }
        const auto graphDurationFrames =
            std::max<std::int64_t>(1, plan.durationFrames);
        auto graph = std::make_unique<Graph>(Graph{
            std::make_unique<Mlt::Profile>(), nullptr, {}, {}, {}, {}, {}, {},
            plan.revision, plan.frameRate, graphDurationFrames,
            0, 0, plan.visualBranches.size(), 0, 0, 0});
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
        const auto isAudible = [](const MltGraphTrack& track) {
            return std::any_of(track.clips.begin(), track.clips.end(),
                               [](const MltGraphClip& clip) {
                                   return clip.enabled && clip.available;
                               });
        };
        std::vector<std::pair<const MltGraphTrack*, int>> nativeAudioTracks;
        const auto appendAudioTrack = [&](const MltGraphTrack& track)
            -> core::Result<int> {
            auto playlist = std::make_unique<Mlt::Playlist>(*graph->profile);
            playlist->set("hide", 1);
            int cursor = 0;
            for (const auto& clip : track.clips) {
                const int timelineIn = static_cast<int>(clip.timelineIn);
                const int length = static_cast<int>(
                    clip.timelineOut - clip.timelineIn + 1);
                if (timelineIn > cursor) playlist->blank(timelineIn - cursor - 1);
                if (!clip.enabled || !clip.available) {
                    playlist->blank(length - 1);
                } else {
                    auto producer = std::make_unique<Mlt::Producer>(
                        *graph->profile, "avformat",
                        utf8Path(clip.mediaPath).c_str());
                    producer->set("threads", 1);
                    producer->set("astream", 0);
                    producer->set("vstream", -1);
                    if (!producer->is_valid()) {
                        return stateError("MLT could not open an audio asset");
                    }
                    auto converter = std::make_unique<Mlt::Filter>(
                        *graph->profile, "audioconvert");
                    if (!converter->is_valid() ||
                        producer->attach(*converter) != 0) {
                        return stateError(
                            "MLT could not attach the audio format converter");
                    }
                    graph->filters.push_back(std::move(converter));
                    if (clip.audioEnvelope.has_value()) {
                        auto context = std::make_unique<AudioFilterContext>(
                            *clip.audioEnvelope, plan.frameRate,
                            static_cast<std::uint64_t>(length));
                        auto* contextObserver = context.get();
                        auto creatorFilter = makeCreatorFilter(
                            std::move(context), processAudioFrame);
                        if (!creatorFilter.hasValue()) {
                            return creatorFilter.error();
                        }
                        if (producer->attach(*creatorFilter.value()) != 0) {
                            return stateError(
                                "MLT could not attach the audio envelope processor");
                        }
                        graph->filters.push_back(
                            std::move(creatorFilter).value());
                        graph->audioFilterContexts.push_back(contextObserver);
                        ++graph->audioEnvelopeBranchCount;
                    }
                    if (playlist->append(
                            *producer, static_cast<int>(clip.sourceIn),
                            static_cast<int>(clip.sourceOut)) != 0) {
                        return stateError("MLT could not place an audio asset");
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
            nativeAudioTracks.emplace_back(&track, nativeTrackIndex);
            return nativeTrackIndex;
        };

        std::optional<int> audioBaseTrackIndex;
        const MltGraphTrack* audioBase = nullptr;
        for (const auto& track : plan.audioTracks) {
            auto appended = appendAudioTrack(track);
            if (!appended.hasValue()) return appended.error();
            if (!audioBase && isAudible(track)) {
                audioBase = &track;
                audioBaseTrackIndex = appended.value();
            }
        }

        if (!audioBaseTrackIndex.has_value()) {
            const int silentTrackIndex = trackIndex++;
            auto silence = std::make_unique<Mlt::Producer>(
                *graph->profile, "blank");
            auto silentTrack =
                std::make_unique<Mlt::Playlist>(*graph->profile);
            silentTrack->set("hide", 1);
            if (!silence->is_valid() ||
                silentTrack->append(
                    *silence, 0,
                    static_cast<int>(graphDurationFrames) - 1) != 0 ||
                graph->tractor->set_track(*silentTrack, silentTrackIndex) != 0) {
                return stateError("MLT could not create the silent audio bed");
            }
            audioBaseTrackIndex = silentTrackIndex;
            graph->producers.push_back(std::move(silence));
            graph->playlists.push_back(std::move(silentTrack));
        }

        const int backgroundTrackIndex = trackIndex++;
        auto background = std::make_unique<Mlt::Producer>(
            *graph->profile, "colour", "black");
        auto backgroundTrack =
            std::make_unique<Mlt::Playlist>(*graph->profile);
        backgroundTrack->set("hide", 2);
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

        std::vector<int> visualTrackIndices;
        visualTrackIndices.reserve(plan.visualBranches.size());
        for (const auto& branch : plan.visualBranches) {
            auto playlist = std::make_unique<Mlt::Playlist>(*graph->profile);
            playlist->set("hide", 2);
            const int timelineIn = static_cast<int>(branch.timelineIn);
            const int length = static_cast<int>(
                branch.timelineOut - branch.timelineIn + 1);
            if (timelineIn > 0) playlist->blank(timelineIn - 1);
            if (!branch.enabled || !branch.available) {
                playlist->blank(length - 1);
                if (branch.sourceKind == MltVisualSourceKind::Generated &&
                    !branch.available) {
                    ++graph->missingOverlayCount;
                }
            } else {
                auto producer = std::make_unique<Mlt::Producer>(
                    *graph->profile, "avformat",
                    utf8Path(branch.sourcePath).c_str());
                producer->set("threads", 1);
                producer->set("astream", -1);
                producer->set("vstream", 0);
                if (!producer->is_valid()) {
                    return stateError("MLT could not open a visual branch");
                }
                const bool transformed =
                    !isIdentityTransform(branch.transform);
                const bool extractsPackedAlpha =
                    requiresPackedAlphaExtraction(branch);
                if (transformed || extractsPackedAlpha) {
                    auto converter = std::make_unique<Mlt::Filter>(
                        *graph->profile, "imageconvert");
                    if (!converter->is_valid() ||
                        producer->attach(*converter) != 0) {
                        return stateError(
                            "MLT could not attach the visual format converter");
                    }
                    graph->filters.push_back(std::move(converter));
                    auto context = std::make_unique<VisualFilterContext>(
                        branch.transform, config_.previewWidth,
                        config_.previewHeight);
                    auto* contextObserver = context.get();
                    auto creatorFilter = makeCreatorFilter(
                        std::move(context), processVisualFrame);
                    if (!creatorFilter.hasValue()) {
                        return creatorFilter.error();
                    }
                    if (producer->attach(*creatorFilter.value()) != 0) {
                        return stateError(
                            "MLT could not attach the visual branch processor");
                    }
                    graph->filters.push_back(
                        std::move(creatorFilter).value());
                    graph->visualFilterContexts.push_back(contextObserver);
                    if (transformed) {
                        ++graph->transformedVisualBranchCount;
                    }
                }
                if (branch.mediaKind == domain::MediaKind::Image) {
                    if (playlist->append(*producer, 0, 0) != 0 ||
                        playlist->repeat(playlist->count() - 1, length) != 0) {
                        return stateError(
                            "MLT could not hold a visual still branch");
                    }
                } else if (playlist->append(
                               *producer, static_cast<int>(branch.sourceIn),
                               static_cast<int>(branch.sourceOut)) != 0) {
                    return stateError("MLT could not place a visual branch");
                }
                graph->producers.push_back(std::move(producer));
            }
            const int cursor = timelineIn + length;
            if (graphDurationFrames > cursor) {
                playlist->blank(
                    static_cast<int>(graphDurationFrames) - cursor - 1);
            }
            const int nativeTrackIndex = trackIndex++;
            if (graph->tractor->set_track(*playlist, nativeTrackIndex) != 0) {
                return stateError("MLT could not connect a visual branch");
            }
            graph->playlists.push_back(std::move(playlist));
            visualTrackIndices.push_back(nativeTrackIndex);
        }

        for (const auto& [track, nativeTrackIndex] : nativeAudioTracks) {
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
        for (const int nativeTrackIndex : visualTrackIndices) {
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
                    "MLT could not connect a visual composite transition");
            }
            ++graph->videoCompositeTransitions;
            graph->transitions.push_back(std::move(composite));
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

core::Result<void> MltEditEngine::initializeRuntime(
    const std::filesystem::path& runtimeRoot) {
    auto verified = verifyMltRuntimeManifest(runtimeRoot);
    if (!verified.hasValue()) return verified;
    std::error_code pathError;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, pathError);
    if (pathError) {
        return stateError("MLT runtime root could not be canonicalized");
    }
    auto bound = MltProcessRuntime::instance().bind(root);
    if (!bound.hasValue()) return bound;
    const auto bin = utf8Path(root / "bin");
    const auto data = utf8Path(root / "share/mlt-7");
    mlt_environment_set("MLT_APPDIR", bin.c_str());
    mlt_environment_set("MLT_DATA", data.c_str());
    mlt_environment_set("MLT_PROFILES_PATH", (data + "/profiles").c_str());
    mlt_environment_set("MLT_PRESETS_PATH", (data + "/presets").c_str());
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
    for (auto* context : impl_->graph_->visualFilterContexts) {
        context->lastErrorStage.store(0, std::memory_order_relaxed);
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
    for (const auto* context : impl_->graph_->visualFilterContexts) {
        const int stage =
            context->lastErrorStage.load(std::memory_order_relaxed);
        if (stage != 0) {
            return stateError(
                "Creator visual processor failed at stage " +
                std::to_string(stage));
        }
    }
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
        .audioMixTransitions = impl_->graph_->audioMixTransitions,
        .visualBranchCount = impl_->graph_->visualBranchCount,
        .transformedVisualBranchCount =
            impl_->graph_->transformedVisualBranchCount,
        .audioEnvelopeBranchCount =
            impl_->graph_->audioEnvelopeBranchCount,
        .missingOverlayCount = impl_->graph_->missingOverlayCount};
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
    for (auto* context : impl_->graph_->audioFilterContexts) {
        context->lastErrorStage.store(0, std::memory_order_relaxed);
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
    for (const auto* context : impl_->graph_->audioFilterContexts) {
        const int stage =
            context->lastErrorStage.load(std::memory_order_relaxed);
        if (stage != 0) {
            return stateError(
                "Creator audio processor failed at stage " +
                std::to_string(stage));
        }
    }
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

core::Result<void> MltEditEngine::renderFrozen(
    const edit_engine::RenderRequest& request, std::stop_token stopToken,
    const std::function<bool(edit_engine::RenderJobState, double,
                             core::TimestampNs)>& report) {
    const auto probeRoot = std::filesystem::temp_directory_path() /
                           ("creator-studio-export-preflight-" +
                            core::generateUuidV4());
    struct ProbeCleanup final {
        std::filesystem::path path;
        ~ProbeCleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } probeCleanup{probeRoot};
    auto encoder = ExportEncoderProbe::probe(
        impl_->config_.runtimeRoot, probeRoot, request.preset());
    if (!encoder.hasValue()) return encoder.error();
    if (stopToken.stop_requested()) {
        return stateError("export cancelled during encoder preflight");
    }

    auto loaded = load(request.snapshot());
    if (!loaded.hasValue()) return loaded;
    if (!impl_->graph_ || !impl_->graph_->profile || !impl_->graph_->tractor) {
        return stateError("independent export graph was not constructed");
    }

    const auto partial = request.destination().parent_path() /
                         (".creator-studio-" + request.jobId().value() +
                          ".partial.mp4");
    PartialArtifact artifact{partial};
    const auto encodedPartial = utf8Path(partial);
    {
        Mlt::Consumer consumer(*impl_->graph_->profile, "avformat",
                               encodedPartial.c_str());
        if (!consumer.is_valid()) {
            return stateError("MLT avformat export consumer is unavailable");
        }
        const auto& selected = encoder.value().selected;
        consumer.set("real_time", -1);
        consumer.set("terminate_on_pause", 1);
        consumer.set("f", "mp4");
        consumer.set("vcodec", selected.videoCodec.c_str());
        consumer.set("acodec", encoder.value().audioCodec.c_str());
        consumer.set("pix_fmt",
                     selected.videoCodec == "h264_mf" ? "nv12" : "yuv420p");
        consumer.set("vb",
                     static_cast<std::int64_t>(request.preset().videoBitrate()));
        consumer.set("ab",
                     static_cast<std::int64_t>(request.preset().audioBitrate()));
        consumer.set("frequency", 48'000);
        consumer.set("channels", 2);
        consumer.set("width", static_cast<int>(request.preset().width()));
        consumer.set("height", static_cast<int>(request.preset().height()));
        consumer.set("frame_rate_num",
                     static_cast<int>(request.preset().frameRate().numerator()));
        consumer.set("frame_rate_den",
                     static_cast<int>(request.preset().frameRate().denominator()));
        consumer.set("progressive", 1);
        consumer.set("movflags", "+faststart");
        if (selected.videoCodec == "h264_mf") {
            consumer.set("hw_encoding",
                         selected.forceMediaFoundationHardware ? 1 : 0);
        }
        if (consumer.connect(*impl_->graph_->tractor) != 0) {
            return stateError("MLT could not connect the independent export graph");
        }
        if (consumer.start() != 0) {
            return stateError("MLT export consumer could not start");
        }
        const auto durationFrames = std::max<std::int64_t>(
            impl_->graph_->durationFrames, 1);
        while (!consumer.is_stopped()) {
            if (stopToken.stop_requested()) {
                consumer.stop();
                return stateError("export cancelled during encoding");
            }
            const auto position = std::clamp<std::int64_t>(
                consumer.position(), 0, durationFrames);
            const double fraction = std::min(
                0.998, static_cast<double>(position) /
                           static_cast<double>(durationFrames));
            const auto through = core::frameToTimestamp(
                position, request.snapshot().timeline.frameRate());
            if (!report(edit_engine::RenderJobState::Running, fraction,
                        through)) {
                consumer.stop();
                return stateError("export progress was rejected");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (consumer.stop() != 0) {
            return stateError("MLT export consumer could not join its worker");
        }
    }
    if (stopToken.stop_requested()) {
        return stateError("export cancelled after encoding");
    }

    core::TimestampNs timelineEnd{};
    for (const auto& track : request.snapshot().timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            if (clip.enabled()) {
                timelineEnd = std::max(timelineEnd, clip.timelineRange().end());
            }
        }
    }
    {
        ffmpeg_adapter::FfmpegMediaProbe probe;
        auto media = probe.probe(partial.parent_path(), partial.filename());
        if (!media.hasValue()) return media.error();
        const auto expectedRate = request.preset().frameRate();
        if (media.value().codecName != "h264" ||
            !media.value().video.has_value() ||
            !media.value().audio.has_value() ||
            media.value().video->width !=
                static_cast<int>(request.preset().width()) ||
            media.value().video->height !=
                static_cast<int>(request.preset().height()) ||
            media.value().video->frameRate != expectedRate ||
            media.value().audio->sampleRate != 48'000 ||
            media.value().audio->channels != 2 ||
            std::chrono::abs(media.value().duration -
                             timelineEnd.time_since_epoch()) >
                std::chrono::milliseconds{100}) {
            return stateError("exported MP4 failed H.264/AAC profile validation");
        }
    }
    if (stopToken.stop_requested()) {
        return stateError("export cancelled before publication");
    }
    if (!report(edit_engine::RenderJobState::Publishing, 0.999, timelineEnd)) {
        return stateError("export publication was cancelled");
    }
    if (stopToken.stop_requested()) {
        return stateError("export cancelled at publication boundary");
    }
    auto published = publishAtomically(partial, request.destination(),
                                       request.overwritePolicy());
    if (!published.hasValue()) return published;
    artifact.published();
    return core::ok();
}

core::Result<std::unique_ptr<edit_engine::IRenderJob>> MltEditEngine::render(
    const edit_engine::RenderRequest& request) {
    auto config = impl_->config_;
    config.previewWidth = request.preset().width();
    config.previewHeight = request.preset().height();
    return MltRenderJob::start(
        request, [config = std::move(config)](
                     const edit_engine::RenderRequest& frozen,
                     std::stop_token stopToken,
                     const MltRenderJob::ProgressReporter& report) mutable {
            MltEditEngine independent{config};
            return independent.renderFrozen(frozen, stopToken, report);
        });
}

}  // namespace creator::mlt_adapter
