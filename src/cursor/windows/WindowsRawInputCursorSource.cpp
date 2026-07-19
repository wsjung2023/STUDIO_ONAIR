#include "cursor/windows/WindowsRawInputCursorSource.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace creator::cursor::windows {
namespace {

core::AppError invalid(std::string message) {
    return {core::ErrorCode::InvalidArgument, std::move(message)};
}

core::AppError ioFailure(std::string message) {
    return {core::ErrorCode::IoFailure, std::move(message)};
}

CursorCaptureGeometry virtualDesktopGeometry() {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) return {};
    return CursorCaptureGeometry{left, top, static_cast<std::uint32_t>(width),
                                 static_cast<std::uint32_t>(height)};
}

std::uint8_t buttonOrdinal(USHORT flags) noexcept {
    if ((flags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) return 0;
    if ((flags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) return 1;
    return 2;
}

}  // namespace

class WindowsRawInputCursorSource::Impl final {
public:
    Impl(CursorCaptureGeometry geometry, std::size_t queueCapacity)
        : geometry_(geometry), queueCapacity_(queueCapacity) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stopRequested_ = true;
            if (window_ != nullptr) PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) thread_.join();
    }

    core::Result<void> start() {
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return startupComplete_; });
        if (startupError_.has_value()) return *startupError_;
        return core::ok();
    }

    std::optional<RawCursorSample> poll() {
        std::lock_guard lock(mutex_);
        if (samples_.empty()) return std::nullopt;
        RawCursorSample result = std::move(samples_.front());
        samples_.pop_front();
        return result;
    }

    std::optional<core::AppError> error() const {
        std::lock_guard lock(mutex_);
        return runtimeError_;
    }

    std::uint64_t droppedSamples() const noexcept {
        return droppedSamples_.load(std::memory_order_relaxed);
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam) noexcept {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) return self->handleMessage(window, message, wParam, lParam);
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam,
                          LPARAM lParam) noexcept {
        if (message == WM_INPUT) {
            consumeInput(reinterpret_cast<HRAWINPUT>(lParam));
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run() noexcept {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        className_ = L"CreatorStudio.RawInputCursor." +
                     std::to_wstring(reinterpret_cast<std::uintptr_t>(this));

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className_.c_str();
        if (RegisterClassExW(&windowClass) == 0) {
            failStartup(ioFailure("Windows could not register the Raw Input window"));
            return;
        }

        window_ = CreateWindowExW(0, className_.c_str(), L"Creator Studio Raw Input",
                                  0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                                  this);
        if (window_ == nullptr) {
            UnregisterClassW(className_.c_str(), instance);
            failStartup(ioFailure("Windows could not create the Raw Input window"));
            return;
        }

        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
        device.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
        device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
        device.hwndTarget = window_;
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
            DestroyWindow(window_);
            window_ = nullptr;
            UnregisterClassW(className_.c_str(), instance);
            failStartup(ioFailure("Windows could not register Raw Input mouse capture"));
            return;
        }

        {
            std::lock_guard lock(mutex_);
            startupComplete_ = true;
            ready_.notify_all();
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            std::lock_guard lock(mutex_);
            if (stopRequested_) break;
        }

        RAWINPUTDEVICE unregisterDevice{};
        unregisterDevice.usUsagePage = 0x01;
        unregisterDevice.usUsage = 0x02;
        unregisterDevice.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&unregisterDevice, 1, sizeof(unregisterDevice));
        if (window_ != nullptr) DestroyWindow(window_);
        window_ = nullptr;
        UnregisterClassW(className_.c_str(), instance);
    }

    void failStartup(core::AppError error) noexcept {
        std::lock_guard lock(mutex_);
        startupError_ = std::move(error);
        startupComplete_ = true;
        ready_.notify_all();
    }

    void consumeInput(HRAWINPUT input) noexcept {
        UINT size = 0;
        if (GetRawInputData(input, RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
            size < sizeof(RAWINPUTHEADER)) {
            setRuntimeError(ioFailure("Windows returned an invalid Raw Input packet"));
            return;
        }
        std::vector<std::byte> storage(size);
        if (GetRawInputData(input, RID_INPUT, storage.data(), &size,
                            sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
            setRuntimeError(ioFailure("Windows could not read a Raw Input packet"));
            return;
        }
        const auto* raw = reinterpret_cast<const RAWINPUT*>(storage.data());
        if (raw->header.dwType != RIM_TYPEMOUSE) return;

        POINT point{};
        if (!GetPhysicalCursorPos(&point)) {
            setRuntimeError(ioFailure("Windows could not read the physical cursor position"));
            return;
        }
        const auto x = clampCoordinate(point.x, geometry_.left, geometry_.width);
        const auto y = clampCoordinate(point.y, geometry_.top, geometry_.height);
        const auto timestamp = core::ProjectClock::now();
        const auto& mouse = raw->data.mouse;
        if (mouse.lLastX != 0 || mouse.lLastY != 0) {
            enqueue(RawCursorMoveSample{timestamp, x, y, geometry_.width,
                                        geometry_.height});
        }

        constexpr USHORT downMask = RI_MOUSE_LEFT_BUTTON_DOWN |
                                     RI_MOUSE_RIGHT_BUTTON_DOWN |
                                     RI_MOUSE_MIDDLE_BUTTON_DOWN;
        const USHORT down = mouse.usButtonFlags & downMask;
        if ((down & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
            enqueue(RawCursorClickSample{timestamp, x, y, geometry_.width,
                                         geometry_.height, buttonOrdinal(RI_MOUSE_LEFT_BUTTON_DOWN)});
        }
        if ((down & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) {
            enqueue(RawCursorClickSample{timestamp, x, y, geometry_.width,
                                         geometry_.height, buttonOrdinal(RI_MOUSE_RIGHT_BUTTON_DOWN)});
        }
        if ((down & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) {
            enqueue(RawCursorClickSample{timestamp, x, y, geometry_.width,
                                         geometry_.height, buttonOrdinal(RI_MOUSE_MIDDLE_BUTTON_DOWN)});
        }
    }

    static std::int64_t clampCoordinate(std::int32_t coordinate, std::int32_t origin,
                                        std::uint32_t extent) noexcept {
        const auto relative = static_cast<std::int64_t>(coordinate) - origin;
        const auto maximum = static_cast<std::int64_t>(extent);
        return std::clamp(relative, std::int64_t{0}, maximum);
    }

    void enqueue(RawCursorSample sample) noexcept {
        std::lock_guard lock(mutex_);
        if (samples_.size() >= queueCapacity_) {
            const bool incomingClick = std::holds_alternative<RawCursorClickSample>(sample);
            auto evict = samples_.begin();
            if (incomingClick) {
                evict = std::find_if(samples_.begin(), samples_.end(), [](const auto& queued) {
                    return std::holds_alternative<RawCursorMoveSample>(queued);
                });
                if (evict == samples_.end()) evict = samples_.begin();
            }
            samples_.erase(evict);
            droppedSamples_.fetch_add(1, std::memory_order_relaxed);
        }
        samples_.push_back(std::move(sample));
    }

    void setRuntimeError(core::AppError error) noexcept {
        std::lock_guard lock(mutex_);
        if (!runtimeError_.has_value()) runtimeError_ = std::move(error);
    }

    CursorCaptureGeometry geometry_;
    std::size_t queueCapacity_{};
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<RawCursorSample> samples_;
    std::optional<core::AppError> startupError_;
    std::optional<core::AppError> runtimeError_;
    std::thread thread_;
    HWND window_{};
    std::wstring className_;
    bool startupComplete_{};
    bool stopRequested_{};
    std::atomic<std::uint64_t> droppedSamples_{0};
};

WindowsRawInputCursorSource::WindowsRawInputCursorSource(CursorCaptureGeometry geometry,
                                                         std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(geometry, queueCapacity)) {}

core::Result<std::unique_ptr<WindowsRawInputCursorSource>>
WindowsRawInputCursorSource::create(CursorCaptureGeometry geometry,
                                    std::size_t queueCapacity) {
    if (queueCapacity == 0) return invalid("cursor queue capacity must be positive");
    if (geometry.width == 0 || geometry.height == 0) {
        geometry = virtualDesktopGeometry();
        if (geometry.width == 0 || geometry.height == 0) {
            return ioFailure("Windows returned an invalid virtual desktop geometry");
        }
    }
    auto source = std::unique_ptr<WindowsRawInputCursorSource>(
        new WindowsRawInputCursorSource(geometry, queueCapacity));
    if (auto started = source->impl_->start(); !started.hasValue()) return started.error();
    return source;
}

WindowsRawInputCursorSource::~WindowsRawInputCursorSource() = default;

std::optional<RawCursorSample> WindowsRawInputCursorSource::poll() {
    return impl_->poll();
}

std::optional<core::AppError> WindowsRawInputCursorSource::error() const {
    return impl_->error();
}

std::uint64_t WindowsRawInputCursorSource::droppedSamples() const noexcept {
    return impl_->droppedSamples();
}

}  // namespace creator::cursor::windows
