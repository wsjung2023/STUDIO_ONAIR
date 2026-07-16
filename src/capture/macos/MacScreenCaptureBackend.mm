#include "capture/macos/MacScreenCaptureBackend.h"

#include "capture/NumericCaptureTargetId.h"
#include "core/AppError.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

@interface CSMacCaptureTargetRegistry : NSObject
@property(nonatomic, copy) NSDictionary<NSString*, SCDisplay*>* displays;
@property(nonatomic, copy) NSDictionary<NSString*, SCWindow*>* windows;
@property(nonatomic, copy) NSArray<SCWindow*>* ownWindows;
@end

@implementation CSMacCaptureTargetRegistry
@end

namespace creator::capture::macos {
namespace {

std::string toUtf8(NSString* value) {
    if (value == nil) return {};
    const char* bytes = value.UTF8String;
    return bytes == nullptr ? std::string{} : std::string{bytes};
}

core::AppError nativeFailure(const char* operation, NSError* error) {
    std::string message{operation};
    message += " failed";
    if (error != nil && error.localizedDescription.length > 0) {
        message += ": ";
        message += toUtf8(error.localizedDescription);
    }
    return core::AppError{core::ErrorCode::IoFailure, std::move(message)};
}

std::uint32_t dimension(CGFloat value) {
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<CGFloat>(std::numeric_limits<std::uint32_t>::max())) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::ceil(value));
}

class MacScreenCapturePermission final : public IScreenCapturePermission {
public:
    [[nodiscard]] ScreenCapturePermissionStatus status() const noexcept override {
        return CGPreflightScreenCaptureAccess() ? ScreenCapturePermissionStatus::Granted
                                                : ScreenCapturePermissionStatus::Denied;
    }

    void request(Completion completion) override {
        auto retainedCompletion = std::make_shared<Completion>(std::move(completion));
        dispatch_async(dispatch_get_main_queue(), ^{
          const auto result = CGRequestScreenCaptureAccess()
                                  ? ScreenCapturePermissionStatus::Granted
                                  : ScreenCapturePermissionStatus::Denied;
          (*retainedCompletion)(result);
        });
    }
};

class MacScreenCaptureDiscovery final : public IScreenCaptureDiscovery {
public:
    explicit MacScreenCaptureDiscovery(CSMacCaptureTargetRegistry* registry)
        : registry_(registry) {}

    void enumerate(Completion completion) override {
        auto retainedCompletion = std::make_shared<Completion>(std::move(completion));
        CSMacCaptureTargetRegistry* retainedRegistry = registry_;
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                   completionHandler:^(SCShareableContent* content,
                                                       NSError* error) {
          if (error != nil || content == nil) {
              (*retainedCompletion)(nativeFailure("ScreenCaptureKit discovery", error));
              return;
          }

          NSMutableDictionary<NSString*, SCDisplay*>* displays =
              [NSMutableDictionary dictionary];
          NSMutableDictionary<NSString*, SCWindow*>* windows =
              [NSMutableDictionary dictionary];
          NSMutableArray<SCWindow*>* ownWindows = [NSMutableArray array];
          std::vector<ScreenCaptureTarget> targets;
          targets.reserve(content.displays.count + content.windows.count);

          for (SCDisplay* display in content.displays) {
              auto id = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Display,
                                                   display.displayID);
              if (!id.hasValue()) continue;
              const auto width = dimension(static_cast<CGFloat>(display.width));
              const auto height = dimension(static_cast<CGFloat>(display.height));
              if (width == 0 || height == 0) continue;
              const std::string name =
                  "Display " + std::to_string(static_cast<std::uint64_t>(display.displayID));
              auto target = ScreenCaptureTarget::create(id.value(),
                                                        ScreenCaptureTargetKind::Display,
                                                        name, std::nullopt, width, height);
              if (!target.hasValue()) continue;
              NSString* key = [NSString stringWithUTF8String:id.value().value().c_str()];
              if (key == nil) continue;
              displays[key] = display;
              targets.push_back(std::move(target).value());
          }

          const pid_t ownProcess = NSProcessInfo.processInfo.processIdentifier;
          for (SCWindow* window in content.windows) {
              if (window.owningApplication.processID == ownProcess) {
                  [ownWindows addObject:window];
                  continue;
              }
              auto id = makeNumericCaptureTargetId(ScreenCaptureTargetKind::Window,
                                                   window.windowID);
              if (!id.hasValue()) continue;
              const auto width = dimension(window.frame.size.width);
              const auto height = dimension(window.frame.size.height);
              if (width == 0 || height == 0) continue;

              std::string title = toUtf8(window.title);
              if (title.empty()) {
                  title = "Window " +
                          std::to_string(static_cast<std::uint64_t>(window.windowID));
              }
              std::optional<std::string> applicationName;
              const std::string appName =
                  toUtf8(window.owningApplication.applicationName);
              if (!appName.empty()) applicationName = appName;
              auto target = ScreenCaptureTarget::create(
                  id.value(), ScreenCaptureTargetKind::Window, std::move(title),
                  std::move(applicationName), width, height);
              if (!target.hasValue()) continue;
              NSString* key = [NSString stringWithUTF8String:id.value().value().c_str()];
              if (key == nil) continue;
              windows[key] = window;
              targets.push_back(std::move(target).value());
          }

          @synchronized(retainedRegistry) {
              retainedRegistry.displays = displays;
              retainedRegistry.windows = windows;
              retainedRegistry.ownWindows = ownWindows;
          }
          (*retainedCompletion)(std::move(targets));
        }];
    }

private:
    CSMacCaptureTargetRegistry* __strong registry_;
};

class MacScreenCaptureSourceFactory final : public IScreenCaptureSourceFactory {
public:
    explicit MacScreenCaptureSourceFactory(CSMacCaptureTargetRegistry* registry)
        : registry_(registry) {}

    core::Result<std::unique_ptr<ICaptureSource>> create(
        const domain::CaptureTargetId&, std::shared_ptr<IVideoFrameSink>) override {
        return core::AppError{core::ErrorCode::InvalidState,
                              "ScreenCaptureKit stream adapter is not initialized"};
    }

private:
    CSMacCaptureTargetRegistry* __strong registry_;
};

}  // namespace

MacScreenCaptureBackend makeMacScreenCaptureBackend() {
    CSMacCaptureTargetRegistry* registry = [CSMacCaptureTargetRegistry new];
    registry.displays = @{};
    registry.windows = @{};
    registry.ownWindows = @[];
    return MacScreenCaptureBackend{
        .permission = std::make_unique<MacScreenCapturePermission>(),
        .discovery = std::make_unique<MacScreenCaptureDiscovery>(registry),
        .sourceFactory = std::make_unique<MacScreenCaptureSourceFactory>(registry),
    };
}

}  // namespace creator::capture::macos

