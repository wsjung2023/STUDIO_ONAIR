#include "app/EditorController.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "mlt_adapter/MltEditEngine.h"
#include "mlt_adapter/MltRuntimeManifest.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QThread>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace creator;

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 10'000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void writePpm(const fs::path& path, std::uint8_t red, std::uint8_t green,
              std::uint8_t blue) {
    std::ofstream output(path, std::ios::binary);
    output << "P6\n4 2\n255\n";
    for (int pixel = 0; pixel < 8; ++pixel) {
        output.put(static_cast<char>(red));
        output.put(static_cast<char>(green));
        output.put(static_cast<char>(blue));
    }
}

class UnicodeMediaPackage final {
public:
    UnicodeMediaPackage() {
        root_ = fs::temp_directory_path() /
                fs::path{u8"creator-studio-검증-"} /
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count());
        fs::create_directories(root_ / "media");
        writePpm(root_ / "media/bottom-red.ppm", 255, 0, 0);
        writePpm(root_ / "media/top-blue.ppm", 0, 0, 255);
    }

    ~UnicodeMediaPackage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path& root() const noexcept { return root_; }

private:
    fs::path root_;
};

edit_engine::TimelineSnapshot layeredSnapshot(const fs::path& mediaRoot) {
    const auto rate = core::FrameRate::create(30, 1).value();
    const auto makeAsset = [&](std::string id, std::string path) {
        return domain::MediaAsset::create(
                   domain::AssetId::create(std::move(id)).value(),
                   domain::MediaKind::Image, std::move(path),
                   core::DurationNs{1'000'000'000},
                   domain::VideoAssetMetadata{4, 2, rate}, std::nullopt, 35,
                   "acceptance-fixture",
                   domain::AssetAvailability::Available)
            .value();
    };
    auto bottomAsset = makeAsset("bottom-red", "media/bottom-red.ppm");
    auto topAsset = makeAsset("top-blue", "media/top-blue.ppm");
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("acceptance").value(),
                        "Unicode multitrack acceptance", rate)
                        .value();
    const auto bottomTrack = domain::TrackId::create("bottom").value();
    const auto topTrack = domain::TrackId::create("top").value();
    EXPECT_TRUE(timeline.addTrack(
                            domain::Track::create(bottomTrack,
                                                  domain::TrackKind::Video,
                                                  "Bottom", true, false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            domain::Track::create(topTrack,
                                                  domain::TrackKind::Video,
                                                  "Top", true, false)
                                .value())
                    .hasValue());
    const auto fullRange = domain::TimeRange::create(
                               core::TimestampNs{},
                               core::DurationNs{1'000'000'000})
                               .value();
    const auto topSourceRange = domain::TimeRange::create(
                                    core::TimestampNs{},
                                    core::DurationNs{500'000'000})
                                    .value();
    const auto topTimelineRange = domain::TimeRange::create(
                                      core::TimestampNs{
                                          core::DurationNs{500'000'000}},
                                      core::DurationNs{500'000'000})
                                      .value();
    EXPECT_TRUE(timeline.insertClip(
                            bottomTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("bottom-clip").value(),
                                bottomAsset, fullRange, fullRange, true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            topTrack,
                            domain::Clip::createAsset(
                                domain::ClipId::create("top-clip").value(),
                                topAsset, topSourceRange, topTimelineRange, true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    std::vector<domain::MediaAsset> assets;
    assets.push_back(bottomAsset);
    assets.push_back(topAsset);
    return edit_engine::TimelineSnapshot{
        std::move(timeline), domain::TimelineRevision::create(1).value(),
        std::move(assets), mediaRoot};
}

void expectRed(const QImage& image) {
    ASSERT_FALSE(image.isNull());
    const auto color = image.pixelColor(0, 0);
    EXPECT_GT(color.red(), 239);
    EXPECT_LT(color.green(), 16);
    EXPECT_LT(color.blue(), 16);
}

void expectBlue(const QImage& image) {
    ASSERT_FALSE(image.isNull());
    const auto color = image.pixelColor(0, 0);
    EXPECT_LT(color.red(), 16);
    EXPECT_LT(color.green(), 16);
    EXPECT_GT(color.blue(), 239);
}

TEST(R1MltPreviewAcceptanceTest,
     StagedRuntimePlaysAndSeeksAUnicodeMultitrackPackage) {
    UnicodeMediaPackage package;
    auto snapshot = layeredSnapshot(package.root());
    const auto assets = snapshot.assets;
    auto nativeEngine = std::make_unique<mlt_adapter::MltEditEngine>(
        mlt_adapter::MltEditEngineConfig{
            .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
            .previewWidth = 4,
            .previewHeight = 2});
    app::EditorController controller{std::move(nativeEngine)};

    controller.openSession(assets, std::move(snapshot));

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.hasPreviewFrame();
    })) << controller.statusMessage().toStdString();
    EXPECT_FALSE(controller.previewStale());
    expectRed(controller.previewImage());

    controller.seek(600'000'000);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.playheadNs() == 600'000'000 &&
               controller.hasPreviewFrame();
    })) << controller.statusMessage().toStdString();
    expectBlue(controller.previewImage());

    controller.play();
    ASSERT_TRUE(waitUntil([&] {
        return controller.playing() && controller.playheadNs() > 600'000'000;
    })) << controller.statusMessage().toStdString();
    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    })) << controller.statusMessage().toStdString();
}

TEST(R1MltPreviewAcceptanceTest,
     PhysicalStagedRuntimeRejectsTamperAndForbiddenExtraFile) {
    const fs::path sourceRoot{CS_TEST_STAGED_MLT_ROOT};
    const auto copyRoot = fs::temp_directory_path() /
                          fs::path{u8"creator-studio-MLT-변조-검증"};
    std::error_code cleanupError;
    fs::remove_all(copyRoot, cleanupError);
    fs::copy(sourceRoot, copyRoot, fs::copy_options::recursive);
    const auto originalModule = sourceRoot / "lib/mlt-7/mltcore.dll";
    const auto copiedModule = copyRoot / "lib/mlt-7/mltcore.dll";

    {
        std::ofstream tamper(copiedModule, std::ios::binary | std::ios::app);
        tamper.put('x');
    }
    EXPECT_FALSE(
        mlt_adapter::verifyMltRuntimeManifest(copyRoot).hasValue());

    fs::copy_file(originalModule, copiedModule,
                  fs::copy_options::overwrite_existing);
    ASSERT_TRUE(mlt_adapter::verifyMltRuntimeManifest(copyRoot).hasValue());

    {
        std::ofstream forbidden(copyRoot / "melt.exe", std::ios::binary);
        forbidden << "forbidden executable fixture";
    }
    EXPECT_FALSE(
        mlt_adapter::verifyMltRuntimeManifest(copyRoot).hasValue());
    fs::remove(copyRoot / "melt.exe");
    EXPECT_TRUE(mlt_adapter::verifyMltRuntimeManifest(copyRoot).hasValue());

    fs::remove_all(copyRoot, cleanupError);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
