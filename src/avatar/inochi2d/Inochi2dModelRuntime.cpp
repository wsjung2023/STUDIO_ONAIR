#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include "core/AppError.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace creator::avatar::inochi2d {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

// Keep these declarations in lockstep with the SDK's generated C header. The
// header is intentionally not bundled, but its ABI is stable and explicitly
// uses cdecl on Windows.
#ifdef _WIN32
#define CS_INOCHI_CALL __cdecl
#else
#define CS_INOCHI_CALL
#endif

struct Inochi2dVec2 final {
    float x;
    float y;
};

using PuppetLoad = void* (CS_INOCHI_CALL *)(const char*);
using PuppetFree = void (CS_INOCHI_CALL *)(void*);
using PuppetGetParameters = void** (CS_INOCHI_CALL *)(void*, std::uint32_t*);
using ParameterGetName = const char* (CS_INOCHI_CALL *)(void*);
using ParameterGetDimensions = std::uint32_t (CS_INOCHI_CALL *)(void*);
using ParameterSetValue = void (CS_INOCHI_CALL *)(void*, Inochi2dVec2);
using PuppetUpdate = void (CS_INOCHI_CALL *)(void*, float);
using PuppetDraw = void (CS_INOCHI_CALL *)(void*, float);
using PuppetGetDrawList = void* (CS_INOCHI_CALL *)(void*);
using DrawListGetCommands = void* (CS_INOCHI_CALL *)(void*, std::uint32_t*);
using DrawListGetVertexData = void* (CS_INOCHI_CALL *)(void*, std::uint32_t*);
using DrawListGetIndexData = void* (CS_INOCHI_CALL *)(void*, std::uint32_t*);
using TextureGetWidth = std::uint32_t (CS_INOCHI_CALL *)(void*);
using TextureGetHeight = std::uint32_t (CS_INOCHI_CALL *)(void*);
using TextureGetChannels = std::uint32_t (CS_INOCHI_CALL *)(void*);
using TextureGetPixels = void* (CS_INOCHI_CALL *)(void*);

struct Inochi2dDrawCommand final {
    void* sources[8];
    std::uint32_t state;
    std::uint32_t blendMode;
    std::uint32_t maskMode;
    std::uint32_t allocId;
    std::uint32_t vtxOffset;
    std::uint32_t idxOffset;
    std::uint32_t elemCount;
    std::uint32_t type;
    unsigned char vars[64];
};

struct Inochi2dVertex2 final {
    float x;
    float y;
    float u;
    float v;
};

std::string utf8Path(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

}  // namespace

class Inochi2dModelRuntime::Impl final {
public:
    ~Impl() { close(); }

    Result<void> load(const std::filesystem::path& libraryPath,
                      const std::filesystem::path& modelPath) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(libraryPath, error)) {
            return AppError{ErrorCode::NotFound,
                            "Inochi2D runtime library is not available"};
        }
        if (!std::filesystem::is_regular_file(modelPath, error)) {
            return AppError{ErrorCode::NotFound,
                            "Inochi2D model file is not available"};
        }
#ifdef _WIN32
        library_ = LoadLibraryW(libraryPath.c_str());
        if (library_ == nullptr) {
            return AppError{ErrorCode::NotFound,
                            "Inochi2D runtime could not be loaded"};
        }
#else
        library_ = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) {
            return AppError{ErrorCode::NotFound,
                            "Inochi2D runtime could not be loaded"};
        }
#endif
        if (!resolve()) {
            close();
            return AppError{ErrorCode::UnsupportedVersion,
                            "Inochi2D runtime is missing required C FFI symbols"};
        }
        const auto model = utf8Path(modelPath);
        puppet_ = loadPuppet_(model.c_str());
        if (puppet_ == nullptr) {
            close();
            return AppError{ErrorCode::ParseFailure,
                            "Inochi2D model could not be loaded"};
        }
        return core::ok();
    }

    Result<void> applyParameters(std::span<const AvatarParameterValue> values) {
        if (puppet_ == nullptr) {
            return AppError{ErrorCode::InvalidState,
                            "Inochi2D model runtime is not loaded"};
        }
        std::uint32_t count = 0;
        void** parameters = getParameters_(puppet_, &count);
        if (parameters == nullptr) {
            return AppError{ErrorCode::IoFailure,
                            "Inochi2D model returned no parameter list"};
        }
        for (const auto& value : values) {
            if (!std::isfinite(value.value)) {
                return AppError{ErrorCode::InvalidArgument,
                                "Inochi2D parameter value is not finite"};
            }
            void* matched = nullptr;
            for (std::uint32_t index = 0; index < count; ++index) {
                const char* name = getName_(parameters[index]);
                if (name != nullptr && value.modelParameter == name) {
                    matched = parameters[index];
                    break;
                }
            }
            if (matched == nullptr) {
                return AppError{ErrorCode::NotFound,
                                "Inochi2D model parameter is not present"};
            }
            if (getDimensions_(matched) != 1U) {
                return AppError{ErrorCode::UnsupportedVersion,
                                "Inochi2D multi-dimensional parameter is unsupported"};
            }
            setValue_(matched, Inochi2dVec2{value.value, 0.0F});
        }
        return core::ok();
    }

    Result<void> update(float deltaSeconds) {
        if (puppet_ == nullptr) {
            return AppError{ErrorCode::InvalidState,
                            "Inochi2D model runtime is not loaded"};
        }
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) {
            return AppError{ErrorCode::InvalidArgument,
                            "Inochi2D update delta is invalid"};
        }
        update_(puppet_, deltaSeconds);
        return core::ok();
    }

    Result<std::vector<AvatarSoftwareRenderInput>> renderSnapshot(
        float deltaSeconds) {
        if (puppet_ == nullptr) {
            return AppError{ErrorCode::InvalidState,
                            "Inochi2D model runtime is not loaded"};
        }
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) {
            return AppError{ErrorCode::InvalidArgument,
                            "Inochi2D render delta is invalid"};
        }
        update_(puppet_, deltaSeconds);
        draw_(puppet_, deltaSeconds);
        void* drawList = getDrawList_(puppet_);
        if (drawList == nullptr) {
            return AppError{ErrorCode::IoFailure,
                            "Inochi2D model returned no draw list"};
        }
        std::uint32_t commandCount = 0;
        auto* commands = static_cast<Inochi2dDrawCommand*>(
            getCommands_(drawList, &commandCount));
        std::uint32_t vertexBytes = 0;
        auto* vertexData = static_cast<Inochi2dVertex2*>(
            getVertexData_(drawList, &vertexBytes));
        std::uint32_t indexBytes = 0;
        auto* indexData = static_cast<std::uint32_t*>(
            getIndexData_(drawList, &indexBytes));
        if (commands == nullptr || vertexData == nullptr || indexData == nullptr ||
            vertexBytes % sizeof(Inochi2dVertex2) != 0U ||
            indexBytes % sizeof(std::uint32_t) != 0U) {
            return AppError{ErrorCode::ParseFailure,
                            "Inochi2D draw list has an unsupported buffer layout"};
        }
        const auto vertexCount = vertexBytes / sizeof(Inochi2dVertex2);
        const auto indexCount = indexBytes / sizeof(std::uint32_t);
        std::vector<AvatarMeshVertex> vertices;
        vertices.reserve(vertexCount);
        for (std::uint32_t index = 0; index < vertexCount; ++index) {
            const auto& source = vertexData[index];
            if (!std::isfinite(source.x) || !std::isfinite(source.y) ||
                !std::isfinite(source.u) || !std::isfinite(source.v)) {
                return AppError{ErrorCode::ParseFailure,
                                "Inochi2D draw list contains a non-finite vertex"};
            }
            vertices.push_back({source.x, source.y, source.u, source.v});
        }

        std::vector<AvatarSoftwareRenderInput> batches;
        for (std::uint32_t commandIndex = 0; commandIndex < commandCount;
             ++commandIndex) {
            const auto& command = commands[commandIndex];
            if (command.elemCount == 0U) continue;
            const auto end = static_cast<std::uint64_t>(command.idxOffset) +
                             command.elemCount;
            if (end > indexCount || command.sources[0] == nullptr) {
                return AppError{ErrorCode::ParseFailure,
                                "Inochi2D draw command is outside its buffers"};
            }
            auto texture = copyTexture(command.sources[0]);
            if (!texture.hasValue()) return texture.error();
            AvatarSoftwareRenderInput batch;
            batch.vertices = vertices;
            batch.texture = std::move(texture).value();
            batch.indices.reserve(command.elemCount);
            for (std::uint32_t offset = 0; offset < command.elemCount; ++offset) {
                const auto vertexIndex = static_cast<std::uint64_t>(
                    indexData[command.idxOffset + offset]) + command.vtxOffset;
                if (vertexIndex >= vertexCount) {
                    return AppError{ErrorCode::ParseFailure,
                                    "Inochi2D draw command references an invalid vertex"};
                }
                batch.indices.push_back(static_cast<std::uint32_t>(vertexIndex));
            }
            if (batch.indices.size() % 3U != 0U) {
                return AppError{ErrorCode::ParseFailure,
                                "Inochi2D draw command is not triangle-aligned"};
            }
            batches.push_back(std::move(batch));
        }
        if (batches.empty()) {
            return AppError{ErrorCode::IoFailure,
                            "Inochi2D draw list contains no textured commands"};
        }
        return batches;
    }

    void close() noexcept {
        if (puppet_ != nullptr && freePuppet_ != nullptr) freePuppet_(puppet_);
        puppet_ = nullptr;
#ifdef _WIN32
        if (library_ != nullptr) {
            FreeLibrary(library_);
            library_ = nullptr;
        }
#else
        if (library_ != nullptr) {
            dlclose(library_);
            library_ = nullptr;
        }
#endif
        loadPuppet_ = nullptr;
        freePuppet_ = nullptr;
        getParameters_ = nullptr;
        getName_ = nullptr;
        getDimensions_ = nullptr;
        setValue_ = nullptr;
        update_ = nullptr;
        draw_ = nullptr;
        getDrawList_ = nullptr;
        getCommands_ = nullptr;
        getVertexData_ = nullptr;
        getIndexData_ = nullptr;
        textureGetWidth_ = nullptr;
        textureGetHeight_ = nullptr;
        textureGetChannels_ = nullptr;
        textureGetPixels_ = nullptr;
    }

private:
    Result<AvatarTexture> copyTexture(void* texture) const {
        const auto width = textureGetWidth_(texture);
        const auto height = textureGetHeight_(texture);
        const auto channels = textureGetChannels_(texture);
        if ((channels != 3U && channels != 4U) || width == 0U || height == 0U) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "Inochi2D texture format is unsupported"};
        }
        const auto width64 = static_cast<std::uint64_t>(width);
        const auto height64 = static_cast<std::uint64_t>(height);
        if (width64 > std::numeric_limits<std::uint64_t>::max() /
                          (height64 * channels) ||
            width64 > std::numeric_limits<std::uint64_t>::max() /
                          (height64 * 4U)) {
            return AppError{ErrorCode::InvalidArgument,
                            "Inochi2D texture dimensions overflow storage"};
        }
        const auto sourceBytes = width64 * height64 * channels;
        const auto outputBytes = width64 * height64 * 4U;
        if (sourceBytes > std::numeric_limits<std::size_t>::max() ||
            outputBytes > std::numeric_limits<std::size_t>::max()) {
            return AppError{ErrorCode::InvalidArgument,
                            "Inochi2D texture dimensions overflow storage"};
        }
        const auto* source = static_cast<const std::uint8_t*>(textureGetPixels_(texture));
        if (source == nullptr) {
            return AppError{ErrorCode::IoFailure,
                            "Inochi2D texture returned no pixels"};
        }
        AvatarTexture result{
            width, height,
            std::vector<std::uint8_t>(static_cast<std::size_t>(outputBytes))};
        for (std::size_t index = 0, pixel = 0;
             index < result.bgra.size(); index += 4U, ++pixel) {
            const auto sourceIndex = pixel * channels;
            result.bgra[index] = source[sourceIndex + 2U];
            result.bgra[index + 1U] = source[sourceIndex + 1U];
            result.bgra[index + 2U] = source[sourceIndex];
            result.bgra[index + 3U] = channels == 4U ? source[sourceIndex + 3U] : 255U;
        }
        return result;
    }

    template <typename Function>
    Function symbol(const char* name) const noexcept {
#ifdef _WIN32
        return reinterpret_cast<Function>(GetProcAddress(library_, name));
#else
        return reinterpret_cast<Function>(dlsym(library_, name));
#endif
    }

    bool resolve() noexcept {
        loadPuppet_ = symbol<PuppetLoad>("in_puppet_load");
        freePuppet_ = symbol<PuppetFree>("in_puppet_free");
        getParameters_ = symbol<PuppetGetParameters>("in_puppet_get_parameters");
        getName_ = symbol<ParameterGetName>("in_parameter_get_name");
        getDimensions_ = symbol<ParameterGetDimensions>("in_parameter_get_dimensions");
        setValue_ = symbol<ParameterSetValue>("in_parameter_set_value");
        update_ = symbol<PuppetUpdate>("in_puppet_update");
        draw_ = symbol<PuppetDraw>("in_puppet_draw");
        getDrawList_ = symbol<PuppetGetDrawList>("in_puppet_get_drawlist");
        getCommands_ = symbol<DrawListGetCommands>("in_drawlist_get_commands");
        getVertexData_ = symbol<DrawListGetVertexData>("in_drawlist_get_vertex_data");
        getIndexData_ = symbol<DrawListGetIndexData>("in_drawlist_get_index_data");
        textureGetWidth_ = symbol<TextureGetWidth>("in_texture_get_width");
        textureGetHeight_ = symbol<TextureGetHeight>("in_texture_get_height");
        textureGetChannels_ = symbol<TextureGetChannels>("in_texture_get_channels");
        textureGetPixels_ = symbol<TextureGetPixels>("in_texture_get_pixels");
        return loadPuppet_ != nullptr && freePuppet_ != nullptr &&
               getParameters_ != nullptr && getName_ != nullptr &&
               getDimensions_ != nullptr && setValue_ != nullptr &&
               update_ != nullptr && draw_ != nullptr &&
               getDrawList_ != nullptr && getCommands_ != nullptr &&
               getVertexData_ != nullptr && getIndexData_ != nullptr &&
               textureGetWidth_ != nullptr && textureGetHeight_ != nullptr &&
               textureGetChannels_ != nullptr && textureGetPixels_ != nullptr;
    }

#ifdef _WIN32
    HMODULE library_{nullptr};
#else
    void* library_{nullptr};
#endif
    void* puppet_{nullptr};
    PuppetLoad loadPuppet_{nullptr};
    PuppetFree freePuppet_{nullptr};
    PuppetGetParameters getParameters_{nullptr};
    ParameterGetName getName_{nullptr};
    ParameterGetDimensions getDimensions_{nullptr};
    ParameterSetValue setValue_{nullptr};
    PuppetUpdate update_{nullptr};
    PuppetDraw draw_{nullptr};
    PuppetGetDrawList getDrawList_{nullptr};
    DrawListGetCommands getCommands_{nullptr};
    DrawListGetVertexData getVertexData_{nullptr};
    DrawListGetIndexData getIndexData_{nullptr};
    TextureGetWidth textureGetWidth_{nullptr};
    TextureGetHeight textureGetHeight_{nullptr};
    TextureGetChannels textureGetChannels_{nullptr};
    TextureGetPixels textureGetPixels_{nullptr};
};

Inochi2dModelRuntime::Inochi2dModelRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Inochi2dModelRuntime::~Inochi2dModelRuntime() = default;

Result<std::unique_ptr<Inochi2dModelRuntime>> Inochi2dModelRuntime::open(
    const std::filesystem::path& libraryPath,
    const std::filesystem::path& modelPath) {
    auto impl = std::make_unique<Impl>();
    auto loaded = impl->load(libraryPath, modelPath);
    if (!loaded.hasValue()) return loaded.error();
    return std::unique_ptr<Inochi2dModelRuntime>{
        new Inochi2dModelRuntime{std::move(impl)}};
}

Result<void> Inochi2dModelRuntime::applyParameters(
    std::span<const AvatarParameterValue> parameters) {
    return impl_->applyParameters(parameters);
}

Result<void> Inochi2dModelRuntime::update(float deltaSeconds) {
    return impl_->update(deltaSeconds);
}

Result<std::vector<AvatarSoftwareRenderInput>>
Inochi2dModelRuntime::renderSnapshot(float deltaSeconds) {
    return impl_->renderSnapshot(deltaSeconds);
}

void Inochi2dModelRuntime::close() noexcept { impl_->close(); }

}  // namespace creator::avatar::inochi2d

#undef CS_INOCHI_CALL
