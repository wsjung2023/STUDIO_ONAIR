#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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

    Result<void> load(const std::filesystem::path& runtimeRoot,
                      const std::filesystem::path& modelPath) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(modelPath, error)) {
            return AppError{ErrorCode::NotFound,
                            "Inochi2D model file is not available"};
        }
        auto verified = Inochi2dRuntimeManifest::openVerified(runtimeRoot);
        if (!verified.hasValue()) return verified.error();
        runtime_.emplace(std::move(verified).value());
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
        // A static puppet legitimately exposes no parameter list. That is only a
        // problem when the caller asks to set a parameter -- the lookup below then
        // reports "not present" -- so an empty request renders the base pose.
        if (parameters == nullptr) count = 0;
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
            // A tracker's parameter set will not match every puppet (a static
            // puppet has none; different rigs expose different names). Skip a
            // parameter this model does not expose rather than failing the whole
            // frame, so any model renders and the ones it does expose still drive.
            if (matched == nullptr) {
                continue;
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

        // Optional diagnostic: dump every draw command (state/blend/mask/texture
        // and world-space bounding box) so the puppet's real layer structure can
        // be inspected. Enabled only when CS_INOCHI2D_DUMP is set.
        {
            char dump[8] = {0};
            std::size_t dumpLen = 0;
#ifdef _WIN32
            const bool wantDump =
                getenv_s(&dumpLen, dump, sizeof dump, "CS_INOCHI2D_DUMP") == 0 &&
                dumpLen > 0;
#else
            const char* dumpEnv = std::getenv("CS_INOCHI2D_DUMP");
            const bool wantDump = dumpEnv != nullptr && dumpEnv[0] != '\0';
#endif
            if (wantDump) {
                std::fprintf(stderr,
                             "[i2d-dump] commands=%u vertices=%zu indices=%zu\n",
                             commandCount, vertices.size(),
                             static_cast<std::size_t>(indexCount));
                for (std::uint32_t ci = 0; ci < commandCount; ++ci) {
                    const auto& c = commands[ci];
                    float minx = 1e30F, miny = 1e30F, maxx = -1e30F, maxy = -1e30F;
                    bool has = false;
                    const auto endi =
                        static_cast<std::uint64_t>(c.idxOffset) + c.elemCount;
                    if (endi <= indexCount) {
                        for (std::uint32_t o = 0; o < c.elemCount; ++o) {
                            const auto vi =
                                static_cast<std::uint64_t>(indexData[c.idxOffset + o]) +
                                c.vtxOffset;
                            if (vi < vertexCount) {
                                const auto& v = vertices[static_cast<std::size_t>(vi)];
                                has = true;
                                minx = std::min(minx, v.x);
                                maxx = std::max(maxx, v.x);
                                miny = std::min(miny, v.y);
                                maxy = std::max(maxy, v.y);
                            }
                        }
                    }
                    int tw = 0, th = 0;
                    if (c.sources[0] != nullptr) {
                        tw = static_cast<int>(textureGetWidth_(c.sources[0]));
                        th = static_cast<int>(textureGetHeight_(c.sources[0]));
                    }
                    std::fprintf(stderr,
                                 "[i2d] #%3u state=%u blend=%2u mask=%u elem=%5u "
                                 "tex=%dx%d bbox=(%.0f,%.0f)-(%.0f,%.0f)%s\n",
                                 ci, c.state, c.blendMode, c.maskMode, c.elemCount,
                                 tw, th, has ? minx : 0.F, has ? miny : 0.F,
                                 has ? maxx : 0.F, has ? maxy : 0.F,
                                 has ? "" : " [empty]");
                }
            }
        }

        // Emit EVERY draw command with its real state/blend/mask so the software
        // compositor can run the actual Inochi2D v0.8.7 draw-list protocol:
        // define-mask/masked-draw for clipping and composite-begin/end/blit for
        // offscreen groups (eyes, mouth). Control states (composite begin/end,
        // blit) carry no texture; textured commands (normal, define-mask,
        // masked-draw) carry their geometry. Geometry is only built for commands
        // that actually have a source texture -- everything else is drawn by its
        // group's blit -- so a command the rasteriser cannot represent is emitted
        // as an inert state transition rather than being dropped.
        std::vector<AvatarSoftwareRenderInput> batches;
        batches.reserve(commandCount);
        bool anyTextured = false;
        for (std::uint32_t commandIndex = 0; commandIndex < commandCount;
             ++commandIndex) {
            const auto& command = commands[commandIndex];
            AvatarSoftwareRenderInput batch;
            batch.state = command.state;
            batch.blendMode = command.blendMode;
            batch.maskMode = command.maskMode;
            const auto end = static_cast<std::uint64_t>(command.idxOffset) +
                             command.elemCount;
            if (command.sources[0] != nullptr && command.elemCount > 0U &&
                end <= indexCount) {
                auto texture = copyTexture(command.sources[0]);
                if (texture.hasValue()) {
                    std::vector<std::uint32_t> indices;
                    indices.reserve(command.elemCount);
                    bool inBounds = true;
                    for (std::uint32_t offset = 0; offset < command.elemCount;
                         ++offset) {
                        const auto vertexIndex = static_cast<std::uint64_t>(
                            indexData[command.idxOffset + offset]) +
                            command.vtxOffset;
                        if (vertexIndex >= vertexCount) {
                            inBounds = false;
                            break;
                        }
                        indices.push_back(static_cast<std::uint32_t>(vertexIndex));
                    }
                    if (inBounds && indices.size() % 3U == 0U) {
                        batch.vertices = vertices;
                        batch.indices = std::move(indices);
                        batch.texture = std::move(texture).value();
                        anyTextured = true;
                    }
                }
            }
            batches.push_back(std::move(batch));
        }
        if (!anyTextured) {
            return AppError{ErrorCode::IoFailure,
                            "Inochi2D draw list contains no textured commands"};
        }
        return batches;
    }

    void close() noexcept {
        if (puppet_ != nullptr && freePuppet_ != nullptr) freePuppet_(puppet_);
        puppet_ = nullptr;
        runtime_.reset();
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
        return runtime_.has_value()
                   ? reinterpret_cast<Function>(runtime_->resolveSymbol(name))
                   : nullptr;
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

    std::optional<Inochi2dVerifiedRuntime> runtime_;
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
    const std::filesystem::path& runtimeRoot,
    const std::filesystem::path& modelPath) {
    auto impl = std::make_unique<Impl>();
    auto loaded = impl->load(runtimeRoot, modelPath);
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
