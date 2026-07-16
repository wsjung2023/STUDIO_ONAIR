#include "app/ScreenCaptureController.h"

#include "capture/IScreenCaptureDiscovery.h"
#include "capture/IScreenCapturePermission.h"
#include "capture/IScreenCaptureSourceFactory.h"
#include "capture/ScreenCaptureTypes.h"
#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "fakes/ManualPushCaptureSource.h"
#include "media/MediaTypes.h"

#include <QCoreApplication>
#include <QEvent>

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::app::ScreenCaptureController;
using creator::app::ScreenCaptureState;
using creator::capture::IScreenCaptureDiscovery;
using creator::capture::IScreenCapturePermission;
using creator::capture::IScreenCaptureSourceFactory;
using creator::capture::IVideoFrameSink;
using creator::capture::ScreenCapturePermissionStatus;
using creator::capture::ScreenCaptureTarget;
using creator::capture::ScreenCaptureTargetKind;
using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::ProjectClock;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::CaptureTargetId;
using creator::domain::SourceId;
using creator::fakes::ManualPushCaptureSource;
using creator::media::PixelFormat;
using creator::media::VideoFrame;

void drainQueuedCalls() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
}

ScreenCaptureTarget displayTarget(std::string id = "display:1",
                                  std::string name = "Built-in Display") {
    return ScreenCaptureTarget::create(CaptureTargetId::create(std::move(id)).value(),
                                       ScreenCaptureTargetKind::Display, std::move(name),
                                       std::nullopt, 1920, 1080)
        .value();
}

ScreenCaptureTarget windowTarget() {
    return ScreenCaptureTarget::create(CaptureTargetId::create("window:7").value(),
                                       ScreenCaptureTargetKind::Window, "Slides",
                                       std::optional<std::string>{"Keynote"}, 1280, 720)
        .value();
}

class PermissionFake final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override {
        return current;
    }

    void request(Completion completion) override {
        ++requestCalls;
        pending = std::move(completion);
    }

    void complete(Result<ScreenCapturePermissionStatus> result) {
        auto completion = std::move(*pending);
        pending.reset();
        if (result.hasValue()) current = result.value();
        completion(std::move(result));
    }

    ScreenCapturePermissionStatus current{ScreenCapturePermissionStatus::Unknown};
    int requestCalls{0};
    std::optional<Completion> pending;
};

class DiscoveryFake final : public IScreenCaptureDiscovery {
public:
    void enumerate(Completion completion) override {
        ++enumerateCalls;
        pending = std::move(completion);
    }

    void complete(Result<std::vector<ScreenCaptureTarget>> result) {
        auto completion = std::move(*pending);
        pending.reset();
        completion(std::move(result));
    }

    int enumerateCalls{0};
    std::optional<Completion> pending;
};

class SourceFactoryFake final : public IScreenCaptureSourceFactory {
public:
    Result<std::unique_ptr<creator::capture::ICaptureSource>> create(
        const CaptureTargetId& targetId,
        std::shared_ptr<IVideoFrameSink> sink) override {
        ++createCalls;
        lastTargetId = targetId.value();
        if (nextError) {
            auto error = std::move(*nextError);
            nextError.reset();
            return error;
        }
        auto source = std::make_unique<ManualPushCaptureSource>(
            SourceId::create("preview-screen").value(), "Preview Screen", std::move(sink));
        lastSource = source.get();
        return std::unique_ptr<creator::capture::ICaptureSource>{std::move(source)};
    }

    int createCalls{0};
    std::string lastTargetId;
    ManualPushCaptureSource* lastSource{nullptr};
    std::optional<AppError> nextError;
};

struct ControllerFixture {
    ControllerFixture(ScreenCapturePermissionStatus permissionStatus) {
        auto permission = std::make_unique<PermissionFake>();
        permission->current = permissionStatus;
        permissionFake = permission.get();
        auto discovery = std::make_unique<DiscoveryFake>();
        discoveryFake = discovery.get();
        auto factory = std::make_unique<SourceFactoryFake>();
        factoryFake = factory.get();
        controller = std::make_unique<ScreenCaptureController>(
            std::move(permission), std::move(discovery), std::move(factory));
    }

    void finishDiscovery(std::vector<ScreenCaptureTarget> targets =
                             {displayTarget(), windowTarget()}) {
        discoveryFake->complete(std::move(targets));
        drainQueuedCalls();
    }

    PermissionFake* permissionFake{};
    DiscoveryFake* discoveryFake{};
    SourceFactoryFake* factoryFake{};
    std::unique_ptr<ScreenCaptureController> controller;
};

TEST(ScreenCaptureControllerTest, GrantedInitializationDiscoversAndSelectsFirstTarget) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};

    fixture.controller->initialize();
    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Discovering);
    EXPECT_TRUE(fixture.controller->busy());
    fixture.finishDiscovery();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Ready);
    EXPECT_FALSE(fixture.controller->busy());
    EXPECT_EQ(fixture.controller->targets().size(), 2);
    EXPECT_EQ(fixture.controller->selectedTargetId(), QStringLiteral("display:1"));
}

TEST(ScreenCaptureControllerTest, PermissionDenialIsVisibleAndCanBeRequested) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Denied};
    fixture.controller->initialize();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::PermissionRequired);
    EXPECT_TRUE(fixture.controller->permissionRequired());

    fixture.controller->requestPermission();
    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::CheckingPermission);
    fixture.permissionFake->complete(ScreenCapturePermissionStatus::Granted);
    drainQueuedCalls();
    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Discovering);
    fixture.finishDiscovery({displayTarget()});
    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Ready);
}

TEST(ScreenCaptureControllerTest, PermissionFailurePreservesExactActionableMessage) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Unknown};
    fixture.controller->initialize();
    fixture.controller->requestPermission();

    fixture.permissionFake->complete(
        AppError{ErrorCode::IoFailure, "screen permission service unavailable"});
    drainQueuedCalls();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Error);
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("screen permission service unavailable"));
}

TEST(ScreenCaptureControllerTest, RejectsSelectionOutsideCurrentDiscoverySnapshot) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};
    fixture.controller->initialize();
    fixture.finishDiscovery();

    fixture.controller->selectTarget(QStringLiteral("window:missing"));

    EXPECT_EQ(fixture.controller->selectedTargetId(), QStringLiteral("display:1"));
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("The selected capture target is no longer available"));
}

TEST(ScreenCaptureControllerTest, StartsSelectedPushSourceAndPublishesLiveGeometry) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};
    fixture.controller->initialize();
    fixture.finishDiscovery();
    fixture.controller->selectTarget(QStringLiteral("window:7"));

    fixture.controller->startPreview();

    ASSERT_EQ(fixture.controller->state(), ScreenCaptureState::Previewing);
    ASSERT_NE(fixture.factoryFake->lastSource, nullptr);
    EXPECT_EQ(fixture.factoryFake->lastTargetId, "window:7");
    ASSERT_TRUE(fixture.factoryFake->lastSource
                    ->pushFrame(VideoFrame{.timestamp = TimestampNs{ProjectClock::duration{1}},
                                           .width = 1440,
                                           .height = 900,
                                           .pixelFormat = PixelFormat::Bgra8})
                    .hasValue());

    fixture.controller->pollCapture();

    EXPECT_EQ(fixture.controller->actualWidth(), 1440u);
    EXPECT_EQ(fixture.controller->actualHeight(), 900u);
    EXPECT_EQ(fixture.controller->receivedFrames(), 1u);
}

TEST(ScreenCaptureControllerTest, TerminalSourceFailureStopsAndSurfacesError) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};
    fixture.controller->initialize();
    fixture.finishDiscovery({windowTarget()});
    fixture.controller->startPreview();
    auto* source = fixture.factoryFake->lastSource;
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(source->fail(AppError{ErrorCode::NotFound, "captured window closed"})
                    .hasValue());

    fixture.controller->pollCapture();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Error);
    EXPECT_FALSE(fixture.controller->previewing());
    EXPECT_EQ(fixture.controller->statusMessage(), QStringLiteral("captured window closed"));
}

TEST(ScreenCaptureControllerTest, StopReturnsToReadyAndReleasesSource) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};
    fixture.controller->initialize();
    fixture.finishDiscovery({displayTarget()});
    fixture.controller->startPreview();
    ASSERT_TRUE(fixture.controller->previewing());

    fixture.controller->stopPreview();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Ready);
    EXPECT_FALSE(fixture.controller->previewing());
    EXPECT_EQ(fixture.controller->statusMessage(), QStringLiteral("Preview stopped"));
}

TEST(ScreenCaptureControllerTest, FactoryFailureDoesNotLeaveStartingState) {
    ControllerFixture fixture{ScreenCapturePermissionStatus::Granted};
    fixture.controller->initialize();
    fixture.finishDiscovery({displayTarget()});
    fixture.factoryFake->nextError =
        AppError{ErrorCode::NotFound, "selected display disappeared"};

    fixture.controller->startPreview();

    EXPECT_EQ(fixture.controller->state(), ScreenCaptureState::Error);
    EXPECT_EQ(fixture.controller->statusMessage(),
              QStringLiteral("selected display disappeared"));
}

}  // namespace

