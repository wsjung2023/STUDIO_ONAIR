#include "capture/macos/MacDeviceCaptureBackend.h"

#include "core/AppError.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace creator::capture::macos {
namespace {

std::string toUtf8(NSString* value) {
    if (value == nil) return {};
    const char* bytes = value.UTF8String;
    return bytes == nullptr ? std::string{} : std::string{bytes};
}

AVMediaType mediaType(CaptureDeviceKind kind) {
    return kind == CaptureDeviceKind::Camera ? AVMediaTypeVideo : AVMediaTypeAudio;
}

MediaPermissionStatus mapPermissionStatus(AVAuthorizationStatus status) noexcept {
    switch (status) {
    case AVAuthorizationStatusAuthorized:
        return MediaPermissionStatus::Granted;
    case AVAuthorizationStatusDenied:
        return MediaPermissionStatus::Denied;
    case AVAuthorizationStatusRestricted:
        return MediaPermissionStatus::Restricted;
    case AVAuthorizationStatusNotDetermined:
        return MediaPermissionStatus::Unknown;
    }
    return MediaPermissionStatus::Unknown;
}

NSArray<AVCaptureDeviceType>* cameraDeviceTypes() {
    NSMutableArray<AVCaptureDeviceType>* types =
        [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
    }
    if (@available(macOS 13.0, *)) {
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    }
    return types;
}

AVCaptureDeviceDiscoverySession* discoverySession(CaptureDeviceKind kind) {
    NSArray<AVCaptureDeviceType>* types =
        kind == CaptureDeviceKind::Camera
            ? cameraDeviceTypes()
            : @[ AVCaptureDeviceTypeMicrophone ];
    return [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:types
                                mediaType:mediaType(kind)
                                 position:AVCaptureDevicePositionUnspecified];
}

class HotplugState final {
public:
    void set(IDeviceCaptureBackend::DeviceChangeHandler handler) {
        std::scoped_lock lock{mutex_};
        handler_ = std::move(handler);
    }

    void fire() noexcept {
        try {
            IDeviceCaptureBackend::DeviceChangeHandler handler;
            {
                std::scoped_lock lock{mutex_};
                handler = handler_;
            }
            if (handler) handler();
        } catch (...) {
            // A C++ exception must never unwind through NotificationCenter.
        }
    }

private:
    std::mutex mutex_;
    IDeviceCaptureBackend::DeviceChangeHandler handler_;
};

class MacDeviceCaptureBackend final : public IDeviceCaptureBackend {
public:
    MacDeviceCaptureBackend() : hotplug_(std::make_shared<HotplugState>()) {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto state = hotplug_;
        connectedObserver_ = [center
            addObserverForName:AVCaptureDeviceWasConnectedNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) { state->fire(); }];
        disconnectedObserver_ = [center
            addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) { state->fire(); }];
    }

    ~MacDeviceCaptureBackend() override {
        hotplug_->set({});
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        if (connectedObserver_ != nil) [center removeObserver:connectedObserver_];
        if (disconnectedObserver_ != nil) [center removeObserver:disconnectedObserver_];
    }

    [[nodiscard]] MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept override {
        return mapPermissionStatus(
            [AVCaptureDevice authorizationStatusForMediaType:mediaType(kind)]);
    }

    void requestPermission(CaptureDeviceKind kind,
                           PermissionCompletion completion) override {
        auto retained = std::make_shared<PermissionCompletion>(std::move(completion));
        [AVCaptureDevice requestAccessForMediaType:mediaType(kind)
                                completionHandler:^(BOOL granted) {
          if (!*retained) return;
          const auto status = granted ? MediaPermissionStatus::Granted
                                      : mapPermissionStatus(
                                            [AVCaptureDevice
                                                authorizationStatusForMediaType:mediaType(kind)]);
          (*retained)(status);
        }];
    }

    [[nodiscard]] core::Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) override {
        AVCaptureDeviceDiscoverySession* session = discoverySession(kind);
        if (session == nil) {
            return core::AppError{core::ErrorCode::Unknown,
                                  "AVFoundation device discovery could not be created"};
        }
        AVCaptureDevice* defaultDevice =
            [AVCaptureDevice defaultDeviceWithMediaType:mediaType(kind)];
        NSString* defaultId = defaultDevice.uniqueID;
        std::vector<CaptureDeviceInfo> result;
        result.reserve(static_cast<std::size_t>(session.devices.count));
        for (AVCaptureDevice* device in session.devices) {
            const std::string idValue = toUtf8(device.uniqueID);
            const std::string name = toUtf8(device.localizedName);
            if (idValue.empty() || name.empty() || !device.connected) continue;
            auto id = domain::CaptureDeviceId::create(idValue);
            if (!id.hasValue()) continue;
            auto info = CaptureDeviceInfo::create(
                std::move(id).value(), kind, name,
                defaultId != nil && [device.uniqueID isEqualToString:defaultId]);
            if (info.hasValue()) result.push_back(std::move(info).value());
        }
        return result;
    }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        hotplug_->set(std::move(handler));
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createCamera(
        const domain::CaptureDeviceId&, std::shared_ptr<IVideoFrameSink>) override {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              "AVFoundation camera source is not initialized"};
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createMicrophone(
        const domain::CaptureDeviceId&, std::shared_ptr<IAudioBlockSink>) override {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              "AVFoundation microphone source is not initialized"};
    }

    [[nodiscard]] core::Result<std::unique_ptr<IDeviceCaptureSource>> createSystemAudio(
        std::shared_ptr<IAudioBlockSink>) override {
        return core::AppError{core::ErrorCode::UnsupportedVersion,
                              "ScreenCaptureKit system audio source is not initialized"};
    }

private:
    std::shared_ptr<HotplugState> hotplug_;
    id __strong connectedObserver_{nil};
    id __strong disconnectedObserver_{nil};
};

}  // namespace

std::unique_ptr<IDeviceCaptureBackend> makeMacDeviceCaptureBackend() {
    return std::make_unique<MacDeviceCaptureBackend>();
}

}  // namespace creator::capture::macos
