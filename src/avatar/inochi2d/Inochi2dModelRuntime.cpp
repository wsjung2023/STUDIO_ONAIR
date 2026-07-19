#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include "core/AppError.h"

#include <cmath>
#include <cstdint>
#include <string>

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
    }

private:
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
        return loadPuppet_ != nullptr && freePuppet_ != nullptr &&
               getParameters_ != nullptr && getName_ != nullptr &&
               getDimensions_ != nullptr && setValue_ != nullptr &&
               update_ != nullptr;
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

void Inochi2dModelRuntime::close() noexcept { impl_->close(); }

}  // namespace creator::avatar::inochi2d

#undef CS_INOCHI_CALL
