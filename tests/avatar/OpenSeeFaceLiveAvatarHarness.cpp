// Focused, human-in-the-loop harness that proves the live avatar render path
// is driven by a REAL face tracked by an external OpenSeeFace process over UDP
// — NOT the synthetic provider.
//
// It reuses the exact production components AvatarSceneController uses:
//   OpenSeeFaceTrackingProvider (ITrackingProvider over the OpenSeeFace UDP
//   source + selectPrimaryFace) -> raw ExpressionParameters -> placeholder
//   AvatarParameterMapper -> AvatarRenderPipeline -> PlaceholderAvatarRenderer.
//
// It also runs the telemetry-side ExpressionNormalizer + ExpressionSmoother so
// the logged eye/mouth/head values are the same normalized+smoothed signal the
// avatar.motion pipeline records.
//
// Usage: OpenSeeFaceLiveAvatarHarness [seconds] [outDir] [port]
//   Requires facetracker.py already sending UDP to 127.0.0.1:<port>.
//   Prints per-tick params, saves PPM frames to outDir, and prints a min/max
//   range summary. A non-zero range on faceFound ticks = the real face moved
//   the avatar.

#include "avatar/AvatarMotionSample.h"
#include "avatar/AvatarParameterMapper.h"
#include "avatar/AvatarRenderPipeline.h"
#include "avatar/CalibrationProfile.h"
#include "avatar/ExpressionNormalizer.h"
#include "avatar/ExpressionParameters.h"
#include "avatar/ExpressionSmoother.h"
#include "avatar/PlaceholderAvatarRenderer.h"
#include "avatar/openseeface/OpenSeeFaceParser.h"
#include "avatar/openseeface/OpenSeeFaceTrackingProvider.h"
#include "core/Timebase.h"
#include "media/MediaTypes.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
namespace av = creator::avatar;
namespace osf = creator::avatar::openseeface;

void savePpm(const std::filesystem::path& path, const av::AvatarRenderFrame& f) {
    // BGRA -> PPM (P6 RGB). Universally viewable; converted to PNG for the
    // report with cv2/PIL afterwards.
    const auto bytes = f.bytes();
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << f.width() << ' ' << f.height() << "\n255\n";
    const auto stride = f.stride();
    for (std::uint32_t y = 0; y < f.height(); ++y) {
        for (std::uint32_t x = 0; x < f.width(); ++x) {
            const auto o = static_cast<std::size_t>(y) * stride + x * 4U;
            const char rgb[3] = {static_cast<char>(bytes[o + 2]),
                                 static_cast<char>(bytes[o + 1]),
                                 static_cast<char>(bytes[o + 0])};
            out.write(rgb, 3);
        }
    }
}

struct Range {
    float mn{1e30F};
    float mx{-1e30F};
    void add(float v) {
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    [[nodiscard]] float span() const { return mx - mn; }
};

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::stod(argv[1]) : 6.0;
    const std::filesystem::path outDir = argc > 2 ? argv[2] : "osf_avatar_shots";
    const auto port = static_cast<std::uint16_t>(argc > 3 ? std::stoi(argv[3])
                                                          : osf::kDefaultUdpPort);
    std::filesystem::create_directories(outDir);

    auto providerRes = osf::OpenSeeFaceTrackingProvider::create(port);
    if (!providerRes.hasValue()) {
        std::fprintf(stderr, "FAIL bind UDP %u: %s\n", port,
                     providerRes.error().message().c_str());
        return 2;
    }
    auto provider = std::move(providerRes).value();
    std::printf("OpenSeeFace UDP provider bound on port %u\n",
                provider->boundPort());

    constexpr std::uint32_t kW = 640;
    constexpr std::uint32_t kH = 480;
    auto mapper = av::AvatarParameterMapper::create(av::placeholderAvatarBindings());
    if (!mapper.hasValue()) {
        std::fprintf(stderr, "FAIL mapper\n");
        return 2;
    }
    av::PlaceholderAvatarRenderer renderer{kW, kH};
    av::AvatarRenderPipeline pipeline{mapper.value(), renderer};

    // Telemetry-side normalize+smooth (identity calibration: no per-performer
    // baseline captured in this harness), to log the same signal avatar.motion
    // would persist.
    av::ExpressionNormalizer normalizer{av::CalibrationProfile::identity()};
    av::ExpressionSmoother smoother{};

    const auto t0 = creator::core::ProjectClock::now();
    const auto wallStart = std::chrono::steady_clock::now();
    const auto tickDt = 33ms;  // ~30 fps, matches AvatarSceneController

    Range rEyeL, rEyeR, rMouthOpen, rMouthWide, rBrowL, rBrowR, rYaw, rPitch, rRoll;
    int ticks = 0;
    int faceTicks = 0;
    int savedShots = 0;
    double nextShotS = 0.5;

    std::printf(
        "  t_s face eyeL eyeR mthOpen mthWide browL browR  yaw pitch roll\n");

    while (true) {
        const auto elapsed = std::chrono::steady_clock::now() - wallStart;
        const double elapsedS =
            std::chrono::duration<double>(elapsed).count();
        if (elapsedS >= seconds) break;

        creator::media::VideoFrame frame{};
        frame.timestamp = t0 + std::chrono::duration_cast<creator::core::DurationNs>(
                                   std::chrono::duration<double>(elapsedS));
        frame.width = kW;
        frame.height = kH;
        frame.pixelFormat = creator::media::PixelFormat::Bgra8;

        auto tracked = provider->process(frame);
        if (!tracked.hasValue()) {
            std::fprintf(stderr, "process error: %s\n",
                         tracked.error().message().c_str());
            std::this_thread::sleep_for(tickDt);
            continue;
        }
        const av::TrackingResult& tr = tracked.value();

        // Render path exactly as AvatarSceneController: raw params -> render.
        const av::AvatarMotionSample rawSample{
            .timestamp = frame.timestamp,
            .parameters = tr.raw,
            .provider = provider->providerId(),
        };
        auto rendered = pipeline.render(rawSample);

        // Telemetry path: normalize+smooth for the logged/persisted signal.
        const auto norm = normalizer.normalize(tr);
        const auto sm = smoother.smooth(norm, frame.timestamp);

        ++ticks;
        if (tr.faceFound) {
            ++faceTicks;
            rEyeL.add(sm.eyeOpenLeft);
            rEyeR.add(sm.eyeOpenRight);
            rMouthOpen.add(sm.mouthOpen);
            rMouthWide.add(sm.mouthWide);
            rBrowL.add(sm.browUpLeft);
            rBrowR.add(sm.browUpRight);
            rYaw.add(sm.headYaw);
            rPitch.add(sm.headPitch);
            rRoll.add(sm.headRoll);
        }

        if (ticks % 5 == 0) {
            std::printf("%6.2f  %d  %.2f %.2f  %.2f    %.2f   %.2f  %.2f  %+.2f %+.2f %+.2f\n",
                        elapsedS, tr.faceFound ? 1 : 0, sm.eyeOpenLeft,
                        sm.eyeOpenRight, sm.mouthOpen, sm.mouthWide, sm.browUpLeft,
                        sm.browUpRight, sm.headYaw, sm.headPitch, sm.headRoll);
        }

        if (rendered.hasValue() && elapsedS >= nextShotS && savedShots < 12) {
            char name[64];
            std::snprintf(name, sizeof(name), "avatar_%02d_t%04.0fms_face%d.ppm",
                          savedShots, elapsedS * 1000.0, tr.faceFound ? 1 : 0);
            savePpm(outDir / name, rendered.value());
            ++savedShots;
            nextShotS += 0.5;
        }

        std::this_thread::sleep_for(tickDt);
    }

    std::printf("\nticks=%d faceFoundTicks=%d savedShots=%d outDir=%s\n", ticks,
                faceTicks, savedShots,
                std::filesystem::absolute(outDir).string().c_str());
    std::printf("field       min     max    range\n");
    auto row = [](const char* n, const Range& r) {
        std::printf("%-10s %7.3f %7.3f %7.3f\n", n, r.mn, r.mx, r.span());
    };
    row("eyeOpenL", rEyeL);
    row("eyeOpenR", rEyeR);
    row("mouthOpen", rMouthOpen);
    row("mouthWide", rMouthWide);
    row("browUpL", rBrowL);
    row("browUpR", rBrowR);
    row("headYaw", rYaw);
    row("headPitch", rPitch);
    row("headRoll", rRoll);

    const bool moved = faceTicks > 0 &&
                       (rMouthOpen.span() > 0.02F || rYaw.span() > 0.02F ||
                        rEyeL.span() > 0.02F || rPitch.span() > 0.02F);
    std::printf("\nRESULT: %s\n",
                (faceTicks > 0 && moved)
                    ? "REAL FACE DROVE AVATAR (faceFound + non-constant params)"
                    : (faceTicks == 0 ? "NO FACE (faceFound stayed 0)"
                                      : "face found but params ~constant"));
    return (faceTicks > 0 && moved) ? 0 : 1;
}
