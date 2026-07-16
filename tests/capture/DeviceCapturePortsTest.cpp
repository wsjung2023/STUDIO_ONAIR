#include "capture/DeviceCaptureTypes.h"
#include "capture/IAudioBlockSink.h"
#include "capture/IDeviceCaptureBackend.h"
#include "capture/IDeviceCaptureSource.h"
#include "capture/UnsupportedDeviceCaptureBackend.h"

#include "domain/Identifiers.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using creator::capture::CaptureDeviceInfo;
using creator::capture::CaptureDeviceKind;
using creator::capture::IDeviceCaptureBackend;
using creator::capture::MediaPermissionStatus;
using creator::core::Result;
using creator::domain::CaptureDeviceId;

CaptureDeviceId deviceId(std::string value = "camera:built-in") {
    return CaptureDeviceId::create(std::move(value)).value();
}

TEST(CaptureDeviceInfoTest, CarriesTypedIdentityKindNameAndDefaultState) {
    const auto result = CaptureDeviceInfo::create(deviceId(), CaptureDeviceKind::Camera,
                                                  "FaceTime HD Camera", true);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().id().value(), "camera:built-in");
    EXPECT_EQ(result.value().kind(), CaptureDeviceKind::Camera);
    EXPECT_EQ(result.value().displayName(), "FaceTime HD Camera");
    EXPECT_TRUE(result.value().isDefault());
}

TEST(CaptureDeviceInfoTest, RejectsAnEmptyDisplayName) {
    const auto result = CaptureDeviceInfo::create(deviceId(), CaptureDeviceKind::Camera, "",
                                                  false);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
}

static_assert(!std::is_same_v<creator::domain::CaptureDeviceId,
                              creator::domain::CaptureTargetId>);
static_assert(!std::is_same_v<creator::domain::CaptureDeviceId,
                              creator::domain::SourceId>);
static_assert(std::is_convertible_v<creator::capture::IDeviceCaptureSource*,
                                    creator::capture::ICaptureSource*>);
static_assert(std::has_virtual_destructor_v<creator::capture::IAudioBlockSink>);

class BackendStub final : public IDeviceCaptureBackend {
public:
    [[nodiscard]] MediaPermissionStatus permissionStatus(
        CaptureDeviceKind kind) const noexcept override {
        return kind == CaptureDeviceKind::Camera ? MediaPermissionStatus::Granted
                                                  : MediaPermissionStatus::Denied;
    }

    void requestPermission(CaptureDeviceKind kind, PermissionCompletion completion) override {
        ++permissionRequests;
        completion(permissionStatus(kind));
    }

    [[nodiscard]] Result<std::vector<CaptureDeviceInfo>> devices(
        CaptureDeviceKind kind) override {
        std::vector<CaptureDeviceInfo> result;
        result.push_back(CaptureDeviceInfo::create(
                             deviceId(kind == CaptureDeviceKind::Camera ? "camera:1" : "mic:1"),
                             kind, kind == CaptureDeviceKind::Camera ? "Camera" : "Microphone",
                             true)
                             .value());
        return result;
    }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        changeHandler = std::move(handler);
    }

    [[nodiscard]] Result<std::unique_ptr<creator::capture::IDeviceCaptureSource>>
    createCamera(const CaptureDeviceId&,
                 std::shared_ptr<creator::capture::IVideoFrameSink>) override {
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "stub has no camera source"};
    }

    [[nodiscard]] Result<std::unique_ptr<creator::capture::IDeviceCaptureSource>>
    createMicrophone(const CaptureDeviceId&,
                     std::shared_ptr<creator::capture::IAudioBlockSink>) override {
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "stub has no microphone source"};
    }

    [[nodiscard]] Result<std::unique_ptr<creator::capture::IDeviceCaptureSource>>
    createSystemAudio(std::shared_ptr<creator::capture::IAudioBlockSink>) override {
        return creator::core::AppError{creator::core::ErrorCode::UnsupportedVersion,
                                       "stub has no system audio source"};
    }

    int permissionRequests{0};
    DeviceChangeHandler changeHandler;
};

TEST(DeviceCaptureBackendTest, ExposesSeparatePermissionAndDeviceSnapshots) {
    BackendStub backend;
    std::optional<Result<MediaPermissionStatus>> permission;

    backend.requestPermission(CaptureDeviceKind::Microphone,
                              [&permission](auto value) { permission.emplace(std::move(value)); });
    const auto cameras = backend.devices(CaptureDeviceKind::Camera);
    const auto microphones = backend.devices(CaptureDeviceKind::Microphone);

    ASSERT_TRUE(permission.has_value());
    ASSERT_TRUE(permission->hasValue());
    EXPECT_EQ(permission->value(), MediaPermissionStatus::Denied);
    ASSERT_TRUE(cameras.hasValue());
    ASSERT_TRUE(microphones.hasValue());
    EXPECT_EQ(cameras.value().front().kind(), CaptureDeviceKind::Camera);
    EXPECT_EQ(microphones.value().front().kind(), CaptureDeviceKind::Microphone);
    EXPECT_EQ(backend.permissionRequests, 1);
}

TEST(DeviceCaptureBackendTest, ChangeHandlerCanBeReplacedAndCleared) {
    BackendStub backend;
    int changes = 0;

    backend.setDeviceChangeHandler([&changes] { ++changes; });
    ASSERT_TRUE(static_cast<bool>(backend.changeHandler));
    backend.changeHandler();
    backend.setDeviceChangeHandler({});

    EXPECT_EQ(changes, 1);
    EXPECT_FALSE(static_cast<bool>(backend.changeHandler));
}

TEST(UnsupportedDeviceCaptureBackendTest, ReportsPlatformSupportErrorsWithoutFakeDevices) {
    creator::capture::UnsupportedDeviceCaptureBackend backend;
    std::optional<Result<MediaPermissionStatus>> permission;

    backend.requestPermission(CaptureDeviceKind::Camera,
                              [&permission](auto value) { permission.emplace(std::move(value)); });
    const auto cameras = backend.devices(CaptureDeviceKind::Camera);

    ASSERT_TRUE(permission.has_value());
    ASSERT_FALSE(permission->hasValue());
    EXPECT_EQ(permission->error().code(), creator::core::ErrorCode::UnsupportedVersion);
    ASSERT_FALSE(cameras.hasValue());
    EXPECT_EQ(cameras.error().code(), creator::core::ErrorCode::UnsupportedVersion);
    EXPECT_EQ(backend.permissionStatus(CaptureDeviceKind::Camera),
              MediaPermissionStatus::Denied);
}

}  // namespace
