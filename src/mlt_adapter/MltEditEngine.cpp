#include "mlt_adapter/MltEditEngine.h"

#include "core/AppError.h"
#include "core/Uuid.h"
#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/AudioProcessingChain.h"
#include "audio_dsp/GainProcessor.h"
#include "audio_dsp/LimiterProcessor.h"
#include "audio_dsp/LoudnessMeter.h"
#include "ffmpeg_adapter/FfmpegConcatRemuxer.h"
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
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <malloc.h>
#endif

namespace creator::mlt_adapter {
namespace {

constexpr std::uint32_t kMaximumPreviewDimension = 8192;

core::AppError stateError(std::string message) {
    return core::AppError{core::ErrorCode::InvalidState, std::move(message)};
}

void releaseTransientExportMemory() noexcept {
    // MLT intentionally pools released image/audio blocks. Between the
    // analysis and encoding passes those blocks are no longer reusable at
    // the same profile size, so retain no value and duplicate peak memory.
    mlt_pool_purge();
#ifdef _WIN32
    static_cast<void>(_heapmin());
    static_cast<void>(SetProcessWorkingSetSize(
        GetCurrentProcess(), static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1)));
#endif
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

core::DurationNs renderDuration(
    const edit_engine::TimelineSnapshot& snapshot) noexcept {
    core::TimestampNs timelineEnd{};
    for (const auto& track : snapshot.timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            timelineEnd = std::max(timelineEnd, clip.timelineRange().end());
        }
    }
    return timelineEnd.time_since_epoch();
}

edit_engine::RenderEncoderDiagnostics encoderDiagnostics(
    const ExportEncoderSelection& selection) {
    std::ostringstream attempted;
    std::ostringstream fallback;
    bool firstAttempt = true;
    bool firstFailure = true;
    for (const auto& attempt : selection.attempts) {
        if (!firstAttempt) attempted << ',';
        firstAttempt = false;
        attempted << attempt.candidate.id;
        if (!attempt.succeeded) {
            if (!firstFailure) fallback << "; ";
            firstFailure = false;
            fallback << attempt.candidate.id << ": " << attempt.diagnostic;
        }
    }
    return {.attemptedEncoders = attempted.str(),
            .selectedEncoder = selection.selected.id,
            .fallbackReason = fallback.str()};
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

struct CursorVisualFilterContext final : CreatorFilterContext {
    CursorVisualFilterContext(std::shared_ptr<const CursorVisualEffectsPlan> value,
                              std::uint32_t width, std::uint32_t height,
                              core::FrameRate rate)
        : plan(std::move(value)), canvasWidth(width), canvasHeight(height),
          frameRate(rate) {}
    std::shared_ptr<const CursorVisualEffectsPlan> plan;
    std::uint32_t canvasWidth;
    std::uint32_t canvasHeight;
    core::FrameRate frameRate;
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

struct AudioProcessingFilterContext final : CreatorFilterContext {
    explicit AudioProcessingFilterContext(
        std::shared_ptr<audio_dsp::IAudioProcessor> processorValue)
        : processor(std::move(processorValue)) {}
    std::shared_ptr<audio_dsp::IAudioProcessor> processor;
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
mlt_frame processCursorVisualFrame(mlt_filter filter, mlt_frame frame);
mlt_frame processAudioFrame(mlt_filter filter, mlt_frame frame);
mlt_frame processAudioProcessingFrame(mlt_filter filter, mlt_frame frame);

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

int getCursorEffectsImage(mlt_frame frame, std::uint8_t** image,
                          mlt_image_format* format, int* width,
                          int* height, int) noexcept {
    auto* filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    auto* context = filter
                        ? static_cast<CursorVisualFilterContext*>(filter->child)
                        : nullptr;
    if (!context || !context->plan || !image || !format || !width || !height) {
        return 1;
    }
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
            return fail(1);
        }
        const auto pixelCount = static_cast<std::uint64_t>(*width) *
                                static_cast<std::uint64_t>(*height);
        if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U) {
            return fail(2);
        }
        const auto byteCount = static_cast<std::size_t>(pixelCount * 4U);
        std::vector<std::uint8_t> bgra(byteCount);
        for (std::size_t offset = 0; offset < byteCount; offset += 4U) {
            bgra[offset] = (*image)[offset + 2U];
            bgra[offset + 1U] = (*image)[offset + 1U];
            bgra[offset + 2U] = (*image)[offset];
            bgra[offset + 3U] = (*image)[offset + 3U];
        }
        const auto position = core::frameToTimestamp(
            mlt_frame_get_position(frame), context->frameRate);
        auto processed = applyCursorVisualEffects(
            BgraFrameView{bgra, static_cast<std::uint32_t>(*width),
                          static_cast<std::uint32_t>(*height),
                          static_cast<std::uint32_t>(*width) * 4U},
            context->canvasWidth, context->canvasHeight, position,
            *context->plan);
        if (!processed.hasValue()) return fail(3);
        const auto output = processed.value().bytes();
        const auto outputPixels = output.size() / 4U;
        if (outputPixels > std::numeric_limits<std::size_t>::max() / 3U ||
            outputPixels > std::numeric_limits<std::size_t>::max()) {
            return fail(4);
        }
        auto* rgb = static_cast<std::uint8_t*>(
            mlt_pool_alloc(static_cast<int>(outputPixels * 3U)));
        auto* alpha = static_cast<std::uint8_t*>(
            mlt_pool_alloc(static_cast<int>(outputPixels)));
        if (!rgb || !alpha) {
            if (rgb) mlt_pool_release(rgb);
            if (alpha) mlt_pool_release(alpha);
            return fail(5);
        }
        for (std::size_t pixel = 0; pixel < outputPixels; ++pixel) {
            const auto source = pixel * 4U;
            const auto destination = pixel * 3U;
            rgb[destination] = output[source + 2U];
            rgb[destination + 1U] = output[source + 1U];
            rgb[destination + 2U] = output[source];
            alpha[pixel] = output[source + 3U];
        }
        if (mlt_frame_set_image(frame, rgb,
                                static_cast<int>(outputPixels * 3U),
                                mlt_pool_release) != 0) {
            mlt_pool_release(rgb);
            mlt_pool_release(alpha);
            return fail(6);
        }
        if (mlt_frame_set_alpha(frame, alpha, static_cast<int>(outputPixels),
                                mlt_pool_release) != 0) {
            mlt_pool_release(alpha);
            return fail(6);
        }
        *image = rgb;
        *format = mlt_image_rgb;
        *width = static_cast<int>(context->canvasWidth);
        *height = static_cast<int>(context->canvasHeight);
        return 0;
    } catch (...) {
        return fail(7);
    }
}

mlt_frame processVisualFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getTransformedImage);
    return frame;
}

mlt_frame processCursorVisualFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getCursorEffectsImage);
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

int getProcessedAudio(mlt_frame frame, void** buffer, mlt_audio_format* format,
                      int* frequency, int* channels, int* samples) noexcept {
    auto* filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    auto* context = filter
                        ? static_cast<AudioProcessingFilterContext*>(filter->child)
                        : nullptr;
    if (!context || !context->processor || !buffer || !format || !frequency ||
        !channels || !samples) {
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
            return fail(1);
        }
        auto audioFormat = audio_dsp::AudioFormat::create(
            static_cast<std::uint32_t>(*frequency),
            static_cast<std::uint32_t>(*channels));
        if (!audioFormat.hasValue()) return fail(2);
        audio_dsp::AudioBuffer audioBuffer{
            static_cast<float*>(*buffer), static_cast<std::size_t>(*samples),
            audioFormat.value()};
        auto processed = context->processor->process(audioBuffer);
        return processed.hasValue() ? 0 : fail(3);
    } catch (...) {
        return fail(4);
    }
}

mlt_frame processAudioFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getEnvelopedAudio));
    return frame;
}

mlt_frame processAudioProcessingFrame(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getProcessedAudio));
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

struct MltMediaSource final {
    Mlt::Producer* borrowedProducer{};

    [[nodiscard]] Mlt::Producer& service() const {
        return *borrowedProducer;
    }
};

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
        std::vector<CursorVisualFilterContext*> cursorVisualFilterContexts;
        std::vector<AudioFilterContext*> audioFilterContexts;
        std::vector<AudioProcessingFilterContext*> audioProcessingFilterContexts;
        domain::TimelineRevision revision;
        core::FrameRate frameRate;
        std::int64_t durationFrames;
        std::size_t mediaProducerCount{};
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
            {}, {},
            plan.revision, plan.frameRate, graphDurationFrames,
            0, 0, 0, plan.visualBranches.size(), 0, 0, 0});
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

        const auto configureProducer = [](Mlt::Producer& producer,
                                          bool audio) {
            producer.set("threads", 1);
            producer.set("autoclose", 1);
            producer.set("cache", 0);
            producer.set("noimagecache", 1);
            producer.set("astream", audio ? 0 : -1);
            producer.set("vstream", audio ? -1 : 0);
        };
        std::map<std::filesystem::path, std::filesystem::path>
            materializedConcatPaths;
        std::map<std::pair<std::string, bool>, Mlt::Producer*>
            reusableMediaProducers;
        const auto buildMediaSource =
            [&](const std::filesystem::path& mediaPath,
                bool audio) -> core::Result<MltMediaSource> {
            auto decoderPath = mediaPath;
            if (mediaPath.extension() == ".ffconcat") {
                const auto cached = materializedConcatPaths.find(mediaPath);
                if (cached != materializedConcatPaths.end()) {
                    decoderPath = cached->second;
                } else {
                    auto materialized =
                        ffmpeg_adapter::materializeFfmpegConcatForEditing(
                            mediaPath);
                    if (!materialized.hasValue()) return materialized.error();
                    decoderPath = std::move(materialized).value();
                    materializedConcatPaths.emplace(mediaPath, decoderPath);
                }
            }
            const auto resource = utf8Path(decoderPath);
            const auto cacheKey = std::pair{resource, audio};
            const auto cached = reusableMediaProducers.find(cacheKey);
            if (cached != reusableMediaProducers.end()) {
                return MltMediaSource{.borrowedProducer = cached->second};
            }
            auto producer = std::make_unique<Mlt::Producer>(
                *graph->profile, "avformat", resource.c_str());
            configureProducer(*producer, audio);
            if (!producer->is_valid()) {
                return stateError("MLT could not open a media asset");
            }
            if (audio) {
                auto converter = std::make_unique<Mlt::Filter>(
                    *graph->profile, "audioconvert");
                if (!converter->is_valid() ||
                    producer->attach(*converter) != 0) {
                    return stateError(
                        "MLT could not attach the audio format converter");
                }
                graph->filters.push_back(std::move(converter));
                auto channelConverter = std::make_unique<Mlt::Filter>(
                    *graph->profile, "audiochannels");
                if (!channelConverter->is_valid() ||
                    producer->attach(*channelConverter) != 0) {
                    return stateError(
                        "MLT could not attach the audio channel converter");
                }
                graph->filters.push_back(std::move(channelConverter));
            }
            ++graph->mediaProducerCount;
            auto* borrowed = producer.get();
            reusableMediaProducers.emplace(cacheKey, borrowed);
            graph->producers.push_back(std::move(producer));
            return MltMediaSource{.borrowedProducer = borrowed};
        };

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
            // Let MLT release each avformat producer after its sequential
            // playlist entry is consumed. Long recordings can contain
            // thousands of short segments; retaining every decoder handle
            // until the graph is destroyed exhausts Windows handles.
            playlist->set("autoclose", 1);
            int cursor = 0;
            for (const auto& clip : track.clips) {
                const int timelineIn = static_cast<int>(clip.timelineIn);
                const int length = static_cast<int>(
                    clip.timelineOut - clip.timelineIn + 1);
                if (timelineIn > cursor) playlist->blank(timelineIn - cursor - 1);
                if (!clip.enabled || !clip.available) {
                    playlist->blank(length - 1);
                } else {
                    auto built = buildMediaSource(clip.mediaPath, true);
                    if (!built.hasValue()) return built.error();
                    auto source = std::move(built).value();
                    auto* producer = &source.service();
                    std::unique_ptr<Mlt::Producer> clipCut;
                    if (clip.audioEnvelope.has_value()) {
                        clipCut.reset(producer->cut(
                            static_cast<int>(clip.sourceIn),
                            static_cast<int>(clip.sourceOut)));
                        if (!clipCut || !clipCut->is_valid()) {
                            return stateError(
                                "MLT could not create an audio clip cut");
                        }
                        producer = clipCut.get();
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
                    const int appended = clipCut
                        ? playlist->append(*producer)
                        : playlist->append(
                              *producer, static_cast<int>(clip.sourceIn),
                              static_cast<int>(clip.sourceOut));
                    if (appended != 0) {
                        return stateError("MLT could not place an audio asset");
                    }
                    if (clipCut) {
                        graph->producers.push_back(std::move(clipCut));
                    }
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
            playlist->set("autoclose", 1);
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
                const bool transformed =
                    !isIdentityTransform(branch.transform);
                const bool extractsPackedAlpha =
                    requiresPackedAlphaExtraction(branch);
                auto built = buildMediaSource(branch.sourcePath, false);
                if (!built.hasValue()) return built.error();
                auto source = std::move(built).value();
                auto& producer = source.service();
                if (transformed || extractsPackedAlpha) {
                    auto converter = std::make_unique<Mlt::Filter>(
                        *graph->profile, "imageconvert");
                    if (!converter->is_valid() ||
                        playlist->attach(*converter) != 0) {
                        return stateError(
                            "MLT could not attach the visual format converter");
                    }
                    converter->set_in_and_out(timelineIn,
                                              timelineIn + length - 1);
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
                    if (playlist->attach(*creatorFilter.value()) != 0) {
                        return stateError(
                            "MLT could not attach the visual branch processor");
                    }
                    creatorFilter.value()->set_in_and_out(
                        timelineIn, timelineIn + length - 1);
                    graph->filters.push_back(
                        std::move(creatorFilter).value());
                    graph->visualFilterContexts.push_back(contextObserver);
                    if (transformed) {
                        ++graph->transformedVisualBranchCount;
                    }
                }
                if (branch.mediaKind == domain::MediaKind::Image) {
                    if (playlist->append(producer, 0, 0) != 0 ||
                        playlist->repeat(playlist->count() - 1, length) != 0) {
                        return stateError(
                            "MLT could not hold a visual still branch");
                    }
                } else {
                    if (playlist->append(
                            producer, static_cast<int>(branch.sourceIn),
                            static_cast<int>(branch.sourceOut)) != 0) {
                        return stateError(
                            "MLT could not place a visual branch");
                    }
                }
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
        if (config_.cursorVisualEffects) {
            auto context = std::make_unique<CursorVisualFilterContext>(
                config_.cursorVisualEffects,
                static_cast<std::uint32_t>(config_.previewWidth),
                static_cast<std::uint32_t>(config_.previewHeight),
                plan.frameRate);
            auto* contextObserver = context.get();
            auto creatorFilter = makeCreatorFilter(
                std::move(context), processCursorVisualFrame);
            if (!creatorFilter.hasValue()) return creatorFilter.error();
            if (graph->tractor->attach(*creatorFilter.value()) != 0) {
                return stateError(
                    "MLT could not attach the cursor visual effects filter");
            }
            graph->filters.push_back(std::move(creatorFilter).value());
            graph->cursorVisualFilterContexts.push_back(contextObserver);
        }
        if (config_.audioProcessingChain && config_.audioProcessingFactory) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "configure either an audio processing chain or a factory, not both"};
        }
        if (config_.audioProcessingChain &&
            config_.appliedExportGainDb.has_value()) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "two-pass loudness requires an isolated audio processor factory"};
        }
        if (config_.appliedExportGainDb.has_value() &&
            !config_.exportLoudnessNormalization.has_value()) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "export loudness gain is missing its limiter configuration"};
        }
        std::unique_ptr<audio_dsp::IAudioProcessor> ownedAudioProcessor;
        std::shared_ptr<audio_dsp::IAudioProcessor> audioProcessor =
            config_.audioProcessingChain;
        if (config_.audioProcessingFactory) {
            auto created = config_.audioProcessingFactory();
            if (!created.hasValue()) return created.error();
            ownedAudioProcessor = std::move(created).value();
            if (!ownedAudioProcessor) {
                return core::AppError{
                    core::ErrorCode::InvalidState,
                    "audio processing factory returned no processor"};
            }
        }
        if (config_.appliedExportGainDb.has_value()) {
            auto format = audio_dsp::AudioFormat::create(48'000, 2);
            if (!format.hasValue()) return format.error();
            audio_dsp::LimiterProcessor::Parameters limiterParameters;
            limiterParameters.ceilingDbtp =
                config_.exportLoudnessNormalization->truePeakCeilingDbtp;
            auto limiter = audio_dsp::LimiterProcessor::create(
                limiterParameters, format.value());
            if (!limiter.hasValue()) return limiter.error();
            auto chain = std::make_unique<audio_dsp::AudioProcessingChain>();
            chain->add(std::move(ownedAudioProcessor));
            chain->add(std::make_unique<audio_dsp::GainProcessor>(
                *config_.appliedExportGainDb));
            chain->add(std::make_unique<audio_dsp::LimiterProcessor>(
                std::move(limiter).value()));
            ownedAudioProcessor = std::move(chain);
        }
        if (ownedAudioProcessor) {
            audioProcessor = std::shared_ptr<audio_dsp::IAudioProcessor>(
                std::move(ownedAudioProcessor));
        }
        if (audioProcessor) {
            auto context = std::make_unique<AudioProcessingFilterContext>(
                std::move(audioProcessor));
            auto* contextObserver = context.get();
            auto creatorFilter = makeCreatorFilter(
                std::move(context), processAudioProcessingFrame);
            if (!creatorFilter.hasValue()) return creatorFilter.error();
            if (graph->tractor->attach(*creatorFilter.value()) != 0) {
                return stateError(
                    "MLT could not attach the configured audio processing chain");
            }
            graph->filters.push_back(std::move(creatorFilter).value());
            graph->audioProcessingFilterContexts.push_back(contextObserver);
        }
        graph->tractor->refresh();
        // Playlist::append retains the producer service.  The extra owning
        // wrappers are only needed while the graph is assembled; keeping one
        // per segment defeats playlist autoclose and exhausts Windows handles
        // during long exports.
        graph->producers.clear();
        return graph;
    }

    core::Result<void> loadSnapshot(
        const edit_engine::TimelineSnapshot& snapshot,
        std::optional<core::FrameRate> outputFrameRate = std::nullopt) {
        auto initialized = initialize();
        if (!initialized.hasValue()) return initialized;
        auto plan = compileMltGraphPlan(snapshot, outputFrameRate);
        if (!plan.hasValue()) return plan.error();
        auto graph = build(plan.value());
        if (!graph.hasValue()) return graph.error();
        graph_ = std::move(graph).value();
        position_ = core::TimestampNs{};
        playing_ = false;
        return core::ok();
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
    return impl_->loadSnapshot(snapshot);
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
    for (auto* context : impl_->graph_->cursorVisualFilterContexts) {
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
    for (const auto* context : impl_->graph_->cursorVisualFilterContexts) {
        const int stage =
            context->lastErrorStage.load(std::memory_order_relaxed);
        if (stage != 0) {
            return stateError(
                "Creator cursor visual effects failed at stage " +
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
        .mediaProducerCount = impl_->graph_->mediaProducerCount,
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
    for (auto* context : impl_->graph_->audioProcessingFilterContexts) {
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
    for (const auto* context : impl_->graph_->audioProcessingFilterContexts) {
        const int stage =
            context->lastErrorStage.load(std::memory_order_relaxed);
        if (stage != 0) {
            return stateError(
                "Creator audio processing chain failed at stage " +
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
    std::vector<float> converted;
    if (format == mlt_audio_f32le) {
        const auto* values = static_cast<const float*>(buffer);
        converted.assign(values, values + sampleCount);
    } else if (format == mlt_audio_s16) {
        const auto* values = static_cast<const std::int16_t*>(buffer);
        converted.resize(sampleCount);
        std::transform(values, values + sampleCount, converted.begin(),
                       [](std::int16_t value) {
                           return static_cast<float>(value) / 32768.0F;
                       });
    } else {
        return stateError("MLT returned an unsupported mixed audio format");
    }
    // The configured chain is attached to the tractor while the graph is
    // built. MLT has already passed this mixed frame through that filter by
    // the time it reaches this conversion boundary; running it again here
    // would double-denoise/double-compress preview audio and export audio.
    return converted;
}

core::Result<void> MltEditEngine::renderFrozen(
    const edit_engine::RenderRequest& request, std::stop_token stopToken,
    const std::function<bool(edit_engine::RenderJobState, double,
                             core::TimestampNs)>& report) {
    if (stopToken.stop_requested()) {
        return stateError("export cancelled before encoder preflight");
    }
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
    if (impl_->config_.renderLifecycle) {
        auto recorded = impl_->config_.renderLifecycle->encoderSelected(
            request.jobId(), encoderDiagnostics(encoder.value()));
        if (!recorded.hasValue()) return recorded.error();
    }
    if (stopToken.stop_requested()) {
        return stateError("export cancelled during encoder preflight");
    }

    constexpr double kMeasurementFraction = 0.1;
    const double encodingStartFraction =
        std::nextafter(kMeasurementFraction, 1.0);
    if (impl_->config_.exportLoudnessNormalization.has_value()) {
        auto format = audio_dsp::AudioFormat::create(48'000, 2);
        if (!format.hasValue()) return format.error();
        auto meter = audio_dsp::LoudnessMeter::create(format.value());
        if (!meter.hasValue()) return meter.error();
        auto analyzer = audio_dsp::ExportLoudnessAnalyzer::create(
            *impl_->config_.exportLoudnessNormalization);
        if (!analyzer.hasValue()) return analyzer.error();

        auto measurementConfig = impl_->config_;
        measurementConfig.renderLifecycle.reset();
        measurementConfig.exportLoudnessNormalization.reset();
        measurementConfig.appliedExportGainDb.reset();
        // The first pass reads mixed audio only. Keeping the 1080p/2160p
        // export profile here needlessly builds and retains full-size video
        // composition buffers before the real encoding pass.
        measurementConfig.previewWidth = 16;
        measurementConfig.previewHeight = 16;
        MltEditEngine measurement{std::move(measurementConfig)};
        auto measuredGraph = measurement.impl_->loadSnapshot(
            request.snapshot(), request.preset().frameRate());
        if (!measuredGraph.hasValue()) return measuredGraph.error();
        if (!measurement.impl_->graph_) {
            return stateError("loudness measurement graph was not constructed");
        }

        const auto durationFrames = std::max<std::int64_t>(
            measurement.impl_->graph_->durationFrames, 1);
        const auto cadence = std::max<std::int64_t>(1, durationFrames / 1000);
        for (std::int64_t frame = 0; frame < durationFrames; ++frame) {
            if (stopToken.stop_requested()) {
                return stateError("export cancelled during loudness measurement");
            }
            const auto firstSample = samplesAtTimelineFrame(
                static_cast<std::uint64_t>(frame), 48'000,
                request.preset().frameRate());
            const auto nextSample = samplesAtTimelineFrame(
                static_cast<std::uint64_t>(frame + 1), 48'000,
                request.preset().frameRate());
            if (!firstSample.has_value() || !nextSample.has_value() ||
                *nextSample <= *firstSample ||
                *nextSample - *firstSample >
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                return stateError("loudness measurement sample range overflowed");
            }
            const auto requestedSamples =
                static_cast<int>(*nextSample - *firstSample);
            auto block = measurement.requestMixedAudio(
                core::frameToTimestamp(frame, request.preset().frameRate()),
                48'000, 2, requestedSamples);
            if (!block.hasValue()) return block.error();
            if (block.value().size() !=
                static_cast<std::size_t>(requestedSamples) * 2U) {
                return stateError(
                    "loudness measurement returned an incomplete audio block");
            }
            audio_dsp::AudioBuffer view{block.value().data(),
                                        block.value().size() / 2U,
                                        format.value()};
            auto added = meter.value().addBlock(view);
            if (!added.hasValue()) return added.error();

            if ((frame + 1) % cadence == 0 || frame + 1 == durationFrames) {
                const double fraction = kMeasurementFraction *
                    static_cast<double>(frame + 1) /
                    static_cast<double>(durationFrames);
                if (!report(edit_engine::RenderJobState::Running, fraction,
                            core::TimestampNs{})) {
                    return stateError("export measurement progress was rejected");
                }
                if (impl_->config_.renderLifecycle) {
                    auto progress = edit_engine::RenderProgress::create(
                        edit_engine::RenderJobState::Running, fraction,
                        core::TimestampNs{}, renderDuration(request.snapshot()));
                    if (!progress.hasValue()) return progress.error();
                    auto recorded = impl_->config_.renderLifecycle->advance(
                        request.jobId(), progress.value());
                    if (!recorded.hasValue()) return recorded.error();
                }
            }
        }
        auto decision = analyzer.value().decide(meter.value());
        if (!decision.hasValue()) return decision.error();
        if (decision.value().shouldNormalize) {
            impl_->config_.appliedExportGainDb = decision.value().gainDb;
        } else {
            impl_->config_.appliedExportGainDb.reset();
        }
        measurement.impl_->graph_.reset();
        releaseTransientExportMemory();
    }

    auto loaded = impl_->loadSnapshot(request.snapshot(),
                                      request.preset().frameRate());
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
        // MLT defaults to a 25-frame read-ahead queue. A 1080p composite
        // frame can retain every source image, so that default duplicates
        // close to a gigabyte while the editor graph remains open.
        consumer.set("buffer", 1);
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
        const auto totalDuration = renderDuration(request.snapshot());
        std::int64_t lastReportedPosition = 0;
        while (!consumer.is_stopped()) {
            if (stopToken.stop_requested()) {
                consumer.stop();
                return stateError("export cancelled during encoding");
            }
            const auto position = std::max(
                lastReportedPosition,
                std::clamp<std::int64_t>(consumer.position(), 0,
                                         durationFrames));
            lastReportedPosition = position;
            const double rawFraction =
                static_cast<double>(position) /
                static_cast<double>(durationFrames);
            const double fraction = std::min(
                0.998,
                impl_->config_.exportLoudnessNormalization.has_value()
                    ? encodingStartFraction +
                          (0.998 - encodingStartFraction) * rawFraction
                    : rawFraction);
            const auto through = std::min(
                core::frameToTimestamp(
                    position, request.snapshot().timeline.frameRate()),
                core::TimestampNs{totalDuration});
            if (!report(edit_engine::RenderJobState::Running, fraction,
                        through)) {
                consumer.stop();
                return stateError("export progress was rejected");
            }
            if (impl_->config_.renderLifecycle) {
                auto progress = edit_engine::RenderProgress::create(
                    edit_engine::RenderJobState::Running, fraction, through,
                    totalDuration);
                if (!progress.hasValue()) {
                    consumer.stop();
                    return progress.error();
                }
                auto recorded = impl_->config_.renderLifecycle->advance(
                    request.jobId(), progress.value());
                if (!recorded.hasValue()) {
                    consumer.stop();
                    return recorded.error();
                }
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

    const core::TimestampNs timelineEnd{renderDuration(request.snapshot())};
    {
        ffmpeg_adapter::FfmpegMediaProbe probe;
        auto media = probe.probe(partial.parent_path(), partial.filename());
        if (!media.hasValue()) return media.error();
        const auto expectedRate = request.preset().frameRate();
        const auto& inspected = media.value();
        const bool validProfile =
            inspected.codecName == "h264" && inspected.video.has_value() &&
            inspected.audio.has_value() &&
            inspected.video->width == static_cast<int>(request.preset().width()) &&
            inspected.video->height == static_cast<int>(request.preset().height()) &&
            inspected.video->frameRate == expectedRate &&
            inspected.audio->sampleRate == 48'000 &&
            inspected.audio->channels == 2 &&
            std::chrono::abs(inspected.duration - timelineEnd.time_since_epoch()) <=
                std::chrono::milliseconds{100};
        if (!validProfile) {
            std::ostringstream diagnostic;
            diagnostic << "exported MP4 failed H.264/AAC profile validation"
                       << "; codec=" << inspected.codecName
                       << "; duration_ns=" << inspected.duration.count()
                       << "; expected_duration_ns="
                       << timelineEnd.time_since_epoch().count();
            if (inspected.video.has_value()) {
                diagnostic << "; video=" << inspected.video->width << 'x'
                           << inspected.video->height << "@"
                           << inspected.video->frameRate.numerator() << '/'
                           << inspected.video->frameRate.denominator();
            } else {
                diagnostic << "; video=missing";
            }
            if (inspected.audio.has_value()) {
                diagnostic << "; audio=" << inspected.audio->sampleRate << '/'
                           << inspected.audio->channels;
            } else {
                diagnostic << "; audio=missing";
            }
            return stateError(diagnostic.str());
        }
    }
    if (stopToken.stop_requested()) {
        return stateError("export cancelled before publication");
    }
    if (!report(edit_engine::RenderJobState::Publishing, 0.999, timelineEnd)) {
        return stateError("export publication was cancelled");
    }
    if (impl_->config_.renderLifecycle) {
        auto progress = edit_engine::RenderProgress::create(
            edit_engine::RenderJobState::Publishing, 0.999, timelineEnd,
            renderDuration(request.snapshot()));
        if (!progress.hasValue()) return progress.error();
        auto prepared = impl_->config_.renderLifecycle->preparePublication(
            request.jobId(), partial, progress.value());
        if (!prepared.hasValue()) return prepared.error();
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
    const auto partial = request.destination().parent_path() /
                         (".creator-studio-" + request.jobId().value() +
                          ".partial.mp4");
    const auto duration = renderDuration(request.snapshot());
    if (config.renderLifecycle) {
        auto begun = config.renderLifecycle->begin(request, partial, duration);
        if (!begun.hasValue()) return begun.error();
    }
    auto started = MltRenderJob::start(
        request, [config = std::move(config)](
                     const edit_engine::RenderRequest& frozen,
                     std::stop_token stopToken,
                     const MltRenderJob::ProgressReporter& report) mutable {
            if (config.renderLifecycle) {
                auto running = edit_engine::RenderProgress::create(
                    edit_engine::RenderJobState::Running, 0.0,
                    core::TimestampNs{}, renderDuration(frozen.snapshot()));
                if (!running.hasValue()) return core::Result<void>{running.error()};
                auto recorded = config.renderLifecycle->advance(
                    frozen.jobId(), running.value());
                if (!recorded.hasValue()) return recorded;
            }
            MltEditEngine independent{config};
            auto rendered = independent.renderFrozen(frozen, stopToken, report);
            if (config.renderLifecycle) {
                const auto state = stopToken.stop_requested()
                                       ? edit_engine::RenderJobState::Cancelled
                                       : rendered.hasValue()
                                             ? edit_engine::RenderJobState::Completed
                                             : edit_engine::RenderJobState::Failed;
                auto finished = config.renderLifecycle->finish(
                    frozen.jobId(), state,
                    rendered.hasValue() ? std::string{}
                                        : rendered.error().message());
                if (!finished.hasValue()) return finished;
            }
            return rendered;
        });
    if (!started.hasValue() && impl_->config_.renderLifecycle) {
        (void)impl_->config_.renderLifecycle->finish(
            request.jobId(), edit_engine::RenderJobState::Failed,
            started.error().message());
    }
    return started;
}

}  // namespace creator::mlt_adapter
