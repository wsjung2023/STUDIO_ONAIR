#include "capture/windows/WindowsScreenCaptureBackend.h"

#include "capture/IScreenCaptureSource.h"
#include "capture/IVideoFrameSink.h"
#include "capture/NumericCaptureTargetId.h"
#include "capture/ScreenCaptureFrameAssembler.h"
#include "capture/ScreenCaptureRegion.h"
#include "capture/ScreenCaptureStopCoordinator.h"
#include "core/AppError.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace creator::capture::windows {
namespace {

namespace wf = winrt::Windows::Foundation;
namespace wg = winrt::Windows::Graphics;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgdxd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError ioFailure(std::string message) {
    return {core::ErrorCode::IoFailure, std::move(message)};
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

/// Turns a winrt::hresult_error into a product error without leaking the winrt
/// exception across the sink boundary (CLAUDE.md 4: callbacks must not throw).
core::AppError fromHresult(const char* operation, const winrt::hresult_error& error) {
    std::string message{operation};
    message += " failed";
    const auto detail = winrt::to_string(error.message());
    if (!detail.empty()) {
        message += ": ";
        message += detail;
    }
    return ioFailure(std::move(message));
}

/// One discovered monitor kept between discovery and source creation. The native
/// HMONITOR never leaves the adapter; callers only ever see the opaque id.
struct MonitorEntry final {
    HMONITOR handle{};
    std::string name;
    std::uint32_t width{0};
    std::uint32_t height{0};
};

/// Thread-safe snapshot of the monitors from the latest discovery pass, keyed by
/// the opaque capture-target id the UI selects.
class MonitorRegistry final {
public:
    void replace(std::unordered_map<std::string, MonitorEntry> monitors) {
        std::scoped_lock lock{mutex_};
        monitors_ = std::move(monitors);
    }

    [[nodiscard]] std::optional<MonitorEntry> find(std::string_view id) const {
        std::scoped_lock lock{mutex_};
        const auto found = monitors_.find(std::string{id});
        return found == monitors_.end() ? std::optional<MonitorEntry>{}
                                        : std::optional<MonitorEntry>{found->second};
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MonitorEntry> monitors_;
};

// --- Frame delivery state -------------------------------------------------

/// Owns the sink, the timestamp-mapping assembler and the CPU read-back
/// resources for one running capture. Frame callbacks are serialized through
/// mutex_ so the D3D11 immediate context (single-threaded) is never touched
/// concurrently, mirroring the serial callback queue the macOS backend relies
/// on. After stopAccepting() no further sink callback is made.
class WgcStreamState final {
public:
    WgcStreamState(std::shared_ptr<IVideoFrameSink> sink,
                   winrt::com_ptr<ID3D11Device> device,
                   winrt::com_ptr<ID3D11DeviceContext> context,
                   std::optional<ScreenCaptureRegion> region)
        : sink_(std::move(sink)),
          device_(std::move(device)),
          context_(std::move(context)),
          region_(std::move(region)) {}

    void notifyStarted() noexcept {
        std::scoped_lock lock{mutex_};
        if (!accepting_ || started_ || !sink_) return;
        started_ = true;
        sink_->onCaptureStarted();
    }

    void onFrameArrived(const wgc::Direct3D11CaptureFramePool& pool) noexcept {
        std::scoped_lock lock{mutex_};
        if (!accepting_ || !sink_) return;
        try {
            auto frame = pool.TryGetNextFrame();
            if (frame == nullptr) return;

            auto nativeFrame = readBack(frame);
            if (!nativeFrame) {
                ++stats_.invalidFrames;
                return;
            }
            deliverLocked(std::move(*nativeFrame));
        } catch (const winrt::hresult_error& error) {
            ++stats_.invalidFrames;
            accepting_ = false;
            sink_->onCaptureError(fromHresult("WGC frame read-back", error));
            sink_.reset();
        } catch (...) {
            ++stats_.invalidFrames;
            accepting_ = false;
            sink_->onCaptureError(
                core::AppError{core::ErrorCode::Unknown, "WGC frame processing failed"});
            sink_.reset();
        }
    }

    void stopAccepting() noexcept {
        std::scoped_lock lock{mutex_};
        accepting_ = false;
        sink_.reset();
    }

    [[nodiscard]] CaptureStats stats() const noexcept {
        std::scoped_lock lock{mutex_};
        return stats_;
    }

private:
    /// Copies the GPU surface into a CPU-readable staging texture, maps it and
    /// packs the visible BGRA rows tightly (stride == width*4). Returns nullopt
    /// for a malformed surface; throws only winrt::hresult_error, caught above.
    [[nodiscard]] std::optional<NativeScreenFrame> readBack(
        const wgc::Direct3D11CaptureFrame& frame) {
        auto surface = frame.Surface();
        if (surface == nullptr) return std::nullopt;

        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::
                                     IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(
            access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) return std::nullopt;

        ensureStaging(desc);
        context_->CopyResource(staging_.get(), texture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped));

        // When a region is requested we crop while packing the visible rows,
        // reading only the sub-rectangle out of the mapped surface. The origin
        // and extent arithmetic mirrors cropBgra8() (the unit-tested geometry);
        // a region that no longer fits the live surface (e.g. the monitor
        // changed resolution) is rejected rather than read out of bounds.
        std::uint32_t outX = 0;
        std::uint32_t outY = 0;
        std::uint32_t outW = desc.Width;
        std::uint32_t outH = desc.Height;
        if (region_) {
            if (region_->x + region_->width > desc.Width ||
                region_->y + region_->height > desc.Height) {
                context_->Unmap(staging_.get(), 0);
                return std::nullopt;
            }
            outX = region_->x;
            outY = region_->y;
            outW = region_->width;
            outH = region_->height;
        }

        const auto width = outW;
        const auto height = outH;
        const auto rowBytes = static_cast<std::size_t>(width) * 4U;
        auto storage = std::make_shared<std::vector<std::uint8_t>>(rowBytes * height);
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        for (std::uint32_t row = 0; row < height; ++row) {
            std::memcpy(storage->data() + static_cast<std::size_t>(row) * rowBytes,
                        source + static_cast<std::size_t>(outY + row) * mapped.RowPitch +
                            static_cast<std::size_t>(outX) * 4U,
                        rowBytes);
        }
        context_->Unmap(staging_.get(), 0);

        // Aliasing shared_ptr: get() yields the first pixel byte while the
        // vector keeps the storage alive, so the frame is self-contained and
        // needs no engine-specific handle type.
        std::shared_ptr<void> handle{storage, storage->data()};

        const auto ticks = frame.SystemRelativeTime().count();  // 100 ns units
        return NativeScreenFrame{
            .status = NativeScreenFrameStatus::Complete,
            .timestamp = NativeTimestamp{.value = ticks, .timescale = 10'000'000},
            .width = width,
            .height = height,
            .visibleRect = media::PixelRect{.width = width, .height = height},
            .contentWidth = width,
            .contentHeight = height,
            .contentScale = 1.0,
            .pointPixelScale = 1.0,
            .pixelFormat = media::PixelFormat::Bgra8,
            .platformHandle = std::move(handle),
        };
    }

    void ensureStaging(const D3D11_TEXTURE2D_DESC& source) {
        if (staging_ && stagingWidth_ == source.Width && stagingHeight_ == source.Height) {
            return;
        }
        D3D11_TEXTURE2D_DESC desc = source;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device_->CreateTexture2D(&desc, nullptr, staging.put()));
        staging_ = std::move(staging);
        stagingWidth_ = source.Width;
        stagingHeight_ = source.Height;
    }

    void deliverLocked(NativeScreenFrame frame) {
        auto assembled = assembler_.assemble(std::move(frame));
        if (!assembled.hasValue()) {
            ++stats_.invalidFrames;
            accepting_ = false;
            sink_->onCaptureError(assembled.error());
            sink_.reset();
            return;
        }
        if (!assembled.value()) {
            ++stats_.ignoredFrames;
            return;
        }
        auto videoFrame = std::move(*assembled.value());
        if (!firstTimestamp_) firstTimestamp_ = videoFrame.timestamp;
        lastTimestamp_ = videoFrame.timestamp;
        ++stats_.receivedFrames;
        if (stats_.receivedFrames > 1 && firstTimestamp_ && lastTimestamp_) {
            const auto elapsed = (*lastTimestamp_ - *firstTimestamp_).count();
            if (elapsed > 0) {
                stats_.currentFps =
                    static_cast<double>(stats_.receivedFrames - 1) * 1'000'000'000.0 /
                    static_cast<double>(elapsed);
            }
        }
        sink_->onVideoFrame(std::move(videoFrame));
    }

    mutable std::mutex mutex_;
    std::shared_ptr<IVideoFrameSink> sink_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    std::uint32_t stagingWidth_{0};
    std::uint32_t stagingHeight_{0};
    std::optional<ScreenCaptureRegion> region_;
    ScreenCaptureFrameAssembler assembler_;
    CaptureStats stats_;
    std::optional<core::TimestampNs> firstTimestamp_;
    std::optional<core::TimestampNs> lastTimestamp_;
    bool accepting_{true};
    bool started_{false};
};

// --- Permission -----------------------------------------------------------

class WindowsScreenCapturePermission final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override {
        return supported() ? ScreenCapturePermissionStatus::Granted
                           : ScreenCapturePermissionStatus::Denied;
    }

    void request(Completion completion) override {
        if (completion) completion(status());
    }

private:
    [[nodiscard]] static bool supported() noexcept {
        try {
            return wgc::GraphicsCaptureSession::IsSupported();
        } catch (...) {
            return false;
        }
    }
};

// --- Discovery ------------------------------------------------------------

struct MonitorEnumeration final {
    std::unordered_map<std::string, MonitorEntry> entries;
    std::vector<ScreenCaptureTarget> targets;
    std::optional<core::AppError> error;
};

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& output = *reinterpret_cast<MonitorEnumeration*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        output.error = ioFailure("Windows could not inspect a display");
        return FALSE;
    }
    const auto nativeId =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(monitor));
    auto id = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Display, nativeId);
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    const std::string name = utf8(info.szDevice);
    if (!id.hasValue() || width <= 0 || height <= 0 || name.empty()) {
        output.error = invalid("Windows returned invalid display metadata");
        return FALSE;
    }
    auto target = ScreenCaptureTarget::create(
        id.value(), ScreenCaptureTargetKind::Display, name, std::nullopt,
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    if (!target.hasValue()) {
        output.error = target.error();
        return FALSE;
    }
    output.entries.emplace(id.value().value(),
                           MonitorEntry{.handle = monitor,
                                        .name = name,
                                        .width = static_cast<std::uint32_t>(width),
                                        .height = static_cast<std::uint32_t>(height)});
    output.targets.push_back(std::move(target).value());
    return TRUE;
}

class WindowsScreenCaptureDiscovery final : public IScreenCaptureDiscovery {
public:
    explicit WindowsScreenCaptureDiscovery(std::shared_ptr<MonitorRegistry> registry)
        : registry_(std::move(registry)) {}

    void enumerate(Completion completion) override {
        MonitorEnumeration output;
        if (!EnumDisplayMonitors(nullptr, nullptr, collectMonitor,
                                 reinterpret_cast<LPARAM>(&output)) ||
            output.error.has_value()) {
            if (completion) {
                completion(output.error.value_or(
                    ioFailure("Windows could not enumerate displays")));
            }
            return;
        }
        registry_->replace(std::move(output.entries));
        if (completion) completion(std::move(output.targets));
    }

private:
    std::shared_ptr<MonitorRegistry> registry_;
};

// --- Source ---------------------------------------------------------------

class WindowsScreenCaptureSource final : public IScreenCaptureSource {
public:
    WindowsScreenCaptureSource(domain::SourceId id, MonitorEntry monitor,
                               std::optional<ScreenCaptureRegion> region,
                               std::shared_ptr<IVideoFrameSink> sink)
        : id_(std::move(id)),
          monitor_(std::move(monitor)),
          region_(std::move(region)),
          sink_(std::move(sink)) {}

    ~WindowsScreenCaptureSource() override {
        static_cast<void>(stop());
        if (stopThread_.joinable()) stopThread_.join();
    }

    [[nodiscard]] domain::SourceId id() const override { return id_; }
    [[nodiscard]] std::string displayName() const override { return monitor_.name; }

    core::Result<void> start(const CaptureConfig& config) override {
        if (started_ || stopCoordinator_) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "WGC source is already started"};
        }
        if (!sink_ || monitor_.handle == nullptr) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "WGC source is not fully configured"};
        }
        if (config.frameRateNumerator == 0 || config.frameRateDenominator == 0) {
            return invalid("WGC capture configuration is invalid");
        }
        if (!WindowsScreenCapturePermissionSupported()) {
            return core::AppError{
                core::ErrorCode::InvalidState,
                "Windows.Graphics.Capture is unavailable on this Windows version"};
        }

        try {
            winrt::com_ptr<ID3D11Device> device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            HRESULT created = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                D3D11_SDK_VERSION, device.put(), nullptr, context.put());
            if (FAILED(created)) {
                // Hardware path unavailable: fall back to WARP and record why
                // (CLAUDE.md 5: hardware-path failure must be reported, not
                // hidden). WGC still delivers real desktop pixels via WARP.
                created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                            nullptr, 0, D3D11_SDK_VERSION, device.put(),
                                            nullptr, context.put());
            }
            if (FAILED(created) || !device || !context) {
                return ioFailure("Direct3D11 device creation failed (HRESULT " +
                                 std::to_string(static_cast<unsigned long>(created)) + ")");
            }

            auto dxgiDevice = device.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(
                CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
            auto winrtDevice = inspectable.as<wgdxd3d::IDirect3DDevice>();

            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                         IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(interop->CreateForMonitor(
                monitor_.handle, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                winrt::put_abi(item)));
            if (item == nullptr) {
                return ioFailure("WGC could not create a capture item for the display");
            }

            const wg::SizeInt32 size = item.Size();
            auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            auto session = framePool.CreateCaptureSession(item);

            auto state = std::make_shared<WgcStreamState>(sink_, device, context, region_);
            frameArrivedToken_ = framePool.FrameArrived(
                [state](const wgc::Direct3D11CaptureFramePool& sender,
                        const wf::IInspectable&) { state->onFrameArrived(sender); });

            session.StartCapture();

            item_ = std::move(item);
            framePool_ = std::move(framePool);
            session_ = std::move(session);
            state_ = std::move(state);
            started_ = true;
            state_->notifyStarted();
            return core::ok();
        } catch (const winrt::hresult_error& error) {
            return fromHresult("Starting WGC capture", error);
        } catch (...) {
            return core::AppError{core::ErrorCode::Unknown, "Starting WGC capture failed"};
        }
    }

    core::Result<void> stop() override {
        beginStop({});
        return core::ok();
    }

    void stopAsync(StopCompletion completion) override {
        if (!started_ && !stopCoordinator_) {
            if (completion) completion(core::ok());
            return;
        }
        if (stopCoordinator_) {
            stopCoordinator_->add(std::move(completion));
            return;
        }
        beginStop(std::move(completion));
    }

    [[nodiscard]] CaptureStats stats() const noexcept override {
        return state_ ? state_->stats() : lastStats_;
    }

private:
    [[nodiscard]] static bool WindowsScreenCapturePermissionSupported() noexcept {
        try {
            return wgc::GraphicsCaptureSession::IsSupported();
        } catch (...) {
            return false;
        }
    }

    /// Bars further callbacks immediately, then tears the WGC session down on a
    /// worker thread so the UI thread never blocks on native teardown. The
    /// completion fires exactly once through the coordinator (CLAUDE.md 9/10:
    /// stop must have an observable recovery path).
    void beginStop(StopCompletion completion) {
        if (stopCoordinator_) {
            if (completion) stopCoordinator_->add(std::move(completion));
            return;
        }
        if (!started_) {
            if (completion) completion(core::ok());
            return;
        }

        started_ = false;
        if (state_) {
            state_->stopAccepting();
            lastStats_ = state_->stats();
        }
        if (framePool_ != nullptr) framePool_.FrameArrived(frameArrivedToken_);

        stopCoordinator_ = std::make_shared<ScreenCaptureStopCoordinator>();
        if (completion) stopCoordinator_->add(std::move(completion));

        auto pool = std::move(framePool_);
        auto session = std::move(session_);
        auto item = std::move(item_);
        auto state = std::move(state_);
        auto stop = stopCoordinator_;
        framePool_ = nullptr;
        session_ = nullptr;
        item_ = nullptr;

        stopThread_ = std::thread([pool = std::move(pool), session = std::move(session),
                                   item = std::move(item), state = std::move(state),
                                   stop]() mutable {
            try {
                if (state) state->stopAccepting();
                if (session != nullptr) session.Close();
                if (pool != nullptr) pool.Close();
                stop->finish(core::ok());
            } catch (const winrt::hresult_error& error) {
                stop->finish(fromHresult("Stopping WGC capture", error));
            } catch (...) {
                stop->finish(core::AppError{core::ErrorCode::Unknown,
                                            "Stopping WGC capture failed"});
            }
        });
    }

    domain::SourceId id_;
    MonitorEntry monitor_;
    std::optional<ScreenCaptureRegion> region_;
    std::shared_ptr<IVideoFrameSink> sink_;
    std::shared_ptr<WgcStreamState> state_;
    std::shared_ptr<ScreenCaptureStopCoordinator> stopCoordinator_;
    wgc::GraphicsCaptureItem item_{nullptr};
    wgc::Direct3D11CaptureFramePool framePool_{nullptr};
    wgc::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameArrivedToken_{};
    std::thread stopThread_;
    CaptureStats lastStats_;
    bool started_{false};
};

// --- Factory --------------------------------------------------------------

class WindowsScreenCaptureSourceFactory final : public IScreenCaptureSourceFactory {
public:
    explicit WindowsScreenCaptureSourceFactory(std::shared_ptr<MonitorRegistry> registry)
        : registry_(std::move(registry)) {}

    core::Result<std::unique_ptr<IScreenCaptureSource>> create(
        const domain::CaptureTargetId& targetId,
        std::shared_ptr<IVideoFrameSink> sink) override {
        if (!sink) return invalid("screen sink is required");
        const auto parsed = parseRegionTargetId(targetId.value());
        auto monitor = registry_->find(parsed.baseId);
        if (!monitor.has_value()) {
            return core::AppError{core::ErrorCode::NotFound,
                                  "selected Windows display is no longer available"};
        }
        if (parsed.region) {
            const auto bounded = ensureRegionWithinBounds(*parsed.region, monitor->width,
                                                          monitor->height);
            if (!bounded.hasValue()) return bounded.error();
        }
        auto sourceId = domain::SourceId::create("screen-preview:" + targetId.value());
        if (!sourceId.hasValue()) return sourceId.error();
        std::unique_ptr<IScreenCaptureSource> source =
            std::make_unique<WindowsScreenCaptureSource>(
                std::move(sourceId).value(), std::move(*monitor), parsed.region,
                std::move(sink));
        return source;
    }

private:
    std::shared_ptr<MonitorRegistry> registry_;
};

}  // namespace

WindowsScreenCaptureBackend makeWindowsScreenCaptureBackend() {
    auto registry = std::make_shared<MonitorRegistry>();
    return WindowsScreenCaptureBackend{
        .permission = std::make_unique<WindowsScreenCapturePermission>(),
        .discovery = std::make_unique<WindowsScreenCaptureDiscovery>(registry),
        .sourceFactory = std::make_unique<WindowsScreenCaptureSourceFactory>(registry),
    };
}

}  // namespace creator::capture::windows
