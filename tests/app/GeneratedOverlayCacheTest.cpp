#include "app/GeneratedOverlayCache.h"

#include "core/Timebase.h"
#include "domain/Timeline.h"

#include <QFile>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

using creator::app::GeneratedOverlayCache;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::CaptionCue;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CueId;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TitlePayload;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

#ifdef _WIN32
bool createDirectoryJunction(const fs::path& link, const fs::path& target) {
    std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.wstring() +
                           L"\" \"" + target.wstring() + L"\" >nul";
    std::vector<wchar_t> mutableCommand{command.begin(), command.end()};
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    const bool readExitCode =
        GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return readExitCode && exitCode == 0;
}
#endif

Timeline generatedTimeline(std::string titleText = "한글 Latin",
                           std::string titleId = "title-clip",
                           std::int64_t cueStart = 100,
                           std::string font = "Arial") {
    auto timeline = Timeline::create(TimelineId::create("main").value(),
                                     "Main", FrameRate::create(60, 1).value())
                        .value();
    const auto titleTrack = TrackId::create("title-1").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(titleTrack, TrackKind::Title,
                                          "Titles", true, false)
                                .value())
                    .hasValue());
    auto title = TitlePayload::create(
                     std::move(titleText), std::move(font), 0.5, 0.35,
                     RgbaColor::parse("#ffffffff").value(),
                     RgbaColor::parse("#112233aa").value(),
                     TextAlignment::Center)
                     .value();
    EXPECT_TRUE(timeline.insertClip(
                            titleTrack,
                            Clip::createTitle(
                                ClipId::create(std::move(titleId)).value(),
                                TimeRange::create(at(0), DurationNs{1'000})
                                    .value(),
                                true, std::move(title), std::nullopt)
                                .value())
                    .hasValue());

    const auto captionTrack = TrackId::create("caption-1").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(captionTrack, TrackKind::Caption,
                                          "Captions", true, false)
                                .value())
                    .hasValue());
    auto cue = CaptionCue::create(CueId::create("cue-1").value(),
                                  DurationNs{cueStart}, DurationNs{300},
                                  "자막 Caption")
                   .value();
    EXPECT_TRUE(timeline.insertClip(
                            captionTrack,
                            Clip::createCaption(
                                ClipId::create("caption-clip").value(),
                                TimeRange::create(at(0), DurationNs{1'000})
                                    .value(),
                                true, {std::move(cue)}, std::nullopt)
                                .value())
                    .hasValue());
    return timeline;
}

class TempDirectory final {
public:
    TempDirectory() {
        path_ = fs::temp_directory_path() /
                ("creator-overlay-cache-" +
                 std::to_string(QCoreApplication::applicationPid()) + "-" +
                 std::to_string(++nextId_));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    const fs::path& path() const noexcept { return path_; }

private:
    inline static std::uint64_t nextId_{0};
    fs::path path_;
};

bool hasVisiblePixel(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) return true;
        }
    }
    return false;
}

TEST(GeneratedOverlayCacheTest, RendersUnicodeDeterministicallyAndReusesPaths) {
    TempDirectory package;
    GeneratedOverlayCache cache;
    const auto frameRate = FrameRate::create(60, 1).value();
    auto first = cache.synchronize(package.path(), generatedTimeline(), 320, 180,
                                   frameRate);
    ASSERT_TRUE(first.hasValue()) << first.error().message();
    ASSERT_EQ(first.value().descriptors.size(), 2U);
    EXPECT_TRUE(first.value().diagnostics.empty());

    const auto titlePath =
        package.path() / first.value().descriptors[0].rasterPath();
    QImage firstImage{QString::fromStdWString(titlePath.wstring())};
    ASSERT_FALSE(firstImage.isNull());
    EXPECT_EQ(firstImage.size(), QSize(320, 180));
    EXPECT_EQ(qAlpha(firstImage.pixel(0, 0)), 0);
    EXPECT_TRUE(hasVisiblePixel(firstImage));

    const auto firstPaths = first.value().descriptors;
    auto reused = cache.synchronize(package.path(), generatedTimeline(), 320,
                                    180, frameRate);
    ASSERT_TRUE(reused.hasValue()) << reused.error().message();
    EXPECT_EQ(reused.value().descriptors, firstPaths);

    ASSERT_TRUE(QFile::remove(QString::fromStdWString(titlePath.wstring())));
    auto rerendered = cache.synchronize(package.path(), generatedTimeline(),
                                        320, 180, frameRate);
    ASSERT_TRUE(rerendered.hasValue()) << rerendered.error().message();
    QImage secondImage{QString::fromStdWString(titlePath.wstring())};
    ASSERT_FALSE(secondImage.isNull());
    EXPECT_EQ(secondImage, firstImage);
}

TEST(GeneratedOverlayCacheTest, FitsUnicodeInsideMinimumAspectCanvas) {
    TempDirectory package;
    GeneratedOverlayCache cache;

    auto rendered = cache.synchronize(package.path(), generatedTimeline(), 64,
                                      36, FrameRate::create(60, 1).value());

    ASSERT_TRUE(rendered.hasValue()) << rendered.error().message();
    ASSERT_EQ(rendered.value().descriptors.size(), 2U);
    for (const auto& descriptor : rendered.value().descriptors) {
        QImage image{QString::fromStdWString(
            (package.path() / descriptor.rasterPath()).wstring())};
        ASSERT_FALSE(image.isNull());
        EXPECT_EQ(image.size(), QSize(64, 36));
        EXPECT_TRUE(hasVisiblePixel(image));
    }
}

TEST(GeneratedOverlayCacheTest, CanonicalKeyCoversIdentityContentTimingAndFormat) {
    TempDirectory package;
    GeneratedOverlayCache cache;
    const auto renderPath = [&](Timeline timeline, int width, int height,
                                FrameRate frameRate) {
        auto result = cache.synchronize(package.path(), std::move(timeline),
                                        width, height, frameRate);
        EXPECT_TRUE(result.hasValue())
            << (result.hasValue() ? "" : result.error().message());
        return result.value().descriptors;
    };
    const auto baseline = renderPath(generatedTimeline(), 320, 180,
                                     FrameRate::create(60, 1).value());
    const auto identity = renderPath(
        generatedTimeline("한글 Latin", "other-title"), 320, 180,
        FrameRate::create(60, 1).value());
    const auto content = renderPath(generatedTimeline("Changed"), 320, 180,
                                    FrameRate::create(60, 1).value());
    const auto style = renderPath(
        generatedTimeline("한글 Latin", "title-clip", 100,
                          "Creator Studio Missing Style Font"),
        320, 180, FrameRate::create(60, 1).value());
    const auto timing = renderPath(
        generatedTimeline("한글 Latin", "title-clip", 200), 320, 180,
        FrameRate::create(60, 1).value());
    const auto canvas = renderPath(generatedTimeline(), 640, 360,
                                   FrameRate::create(60, 1).value());
    const auto rate = renderPath(generatedTimeline(), 320, 180,
                                 FrameRate::create(30, 1).value());

    ASSERT_EQ(baseline.size(), 2U);
    EXPECT_NE(baseline[0].rasterPath(), identity[0].rasterPath());
    EXPECT_NE(baseline[0].rasterPath(), content[0].rasterPath());
    EXPECT_NE(baseline[0].rasterPath(), style[0].rasterPath());
    EXPECT_NE(baseline[1].rasterPath(), timing[1].rasterPath());
    EXPECT_NE(baseline[0].rasterPath(), canvas[0].rasterPath());
    EXPECT_NE(baseline[0].rasterPath(), rate[0].rasterPath());
}

TEST(GeneratedOverlayCacheTest, RepairsInvalidFilesAndCleansOnlyTemporaryFiles) {
    TempDirectory package;
    GeneratedOverlayCache cache;
    auto initial = cache.synchronize(package.path(), generatedTimeline(), 320,
                                     180, FrameRate::create(60, 1).value());
    ASSERT_TRUE(initial.hasValue()) << initial.error().message();
    const auto generatedDirectory = package.path() / "cache" / "generated";
    const auto titlePath =
        package.path() / initial.value().descriptors[0].rasterPath();
    QFile corrupt{QString::fromStdWString(titlePath.wstring())};
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(corrupt.write("not a png"), 9);
    corrupt.close();
    QFile abandoned{QString::fromStdWString(
        (generatedDirectory / "abandoned.tmp").wstring())};
    ASSERT_TRUE(abandoned.open(QIODevice::WriteOnly));
    abandoned.close();
    QFile unrelated{QString::fromStdWString(
        (generatedDirectory /
         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.png")
            .wstring())};
    ASSERT_TRUE(unrelated.open(QIODevice::WriteOnly));
    ASSERT_EQ(unrelated.write("keep"), 4);
    unrelated.close();

    auto repaired = cache.synchronize(package.path(), generatedTimeline(), 320,
                                      180, FrameRate::create(60, 1).value());
    ASSERT_TRUE(repaired.hasValue()) << repaired.error().message();
    EXPECT_FALSE(QImage{QString::fromStdWString(titlePath.wstring())}.isNull());
    EXPECT_FALSE(fs::exists(generatedDirectory / "abandoned.tmp"));
    EXPECT_TRUE(fs::exists(
        generatedDirectory /
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.png"));

    QFile zero{QString::fromStdWString(titlePath.wstring())};
    ASSERT_TRUE(zero.open(QIODevice::WriteOnly | QIODevice::Truncate));
    zero.close();
    auto zeroRepaired = cache.synchronize(
        package.path(), generatedTimeline(), 320, 180,
        FrameRate::create(60, 1).value());
    ASSERT_TRUE(zeroRepaired.hasValue()) << zeroRepaired.error().message();
    EXPECT_GT(fs::file_size(titlePath), 0U);
}

TEST(GeneratedOverlayCacheTest, ResolvesUnavailableFontAndRefusesDirectoryTarget) {
    TempDirectory package;
    GeneratedOverlayCache cache;
    const std::string unavailable =
        "Creator Studio Definitely Missing Font 9F46B4F0";
    auto result = cache.synchronize(
        package.path(), generatedTimeline("Text", "title-clip", 100,
                                          unavailable),
        320, 180, FrameRate::create(60, 1).value());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_FALSE(result.value().descriptors.empty());
    EXPECT_NE(result.value().descriptors[0].resolvedFontFamily(), unavailable);

    const auto path =
        package.path() / result.value().descriptors[0].rasterPath();
    ASSERT_TRUE(fs::remove(path));
    ASSERT_TRUE(fs::create_directory(path));
    auto refused = cache.synchronize(
        package.path(), generatedTimeline("Text", "title-clip", 100,
                                          unavailable),
        320, 180, FrameRate::create(60, 1).value());
    ASSERT_TRUE(refused.hasValue()) << refused.error().message();
    EXPECT_EQ(refused.value().descriptors.size(), 1U);
    EXPECT_EQ(refused.value().diagnostics.size(), 1U);
    EXPECT_TRUE(refused.value().descriptors[0].cueId().has_value());
}

TEST(GeneratedOverlayCacheTest, AtomicCommitFailureKeepsSuccessfulOverlays) {
    TempDirectory package;
    int commitAttempts = 0;
    GeneratedOverlayCache cache{[&](const fs::path&) -> creator::core::Result<void> {
        ++commitAttempts;
        if (commitAttempts == 2) {
            return creator::core::AppError{
                creator::core::ErrorCode::IoFailure,
                "injected atomic commit failure"};
        }
        return creator::core::ok();
    }};
    auto result = cache.synchronize(package.path(), generatedTimeline(), 320,
                                    180, FrameRate::create(60, 1).value());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    ASSERT_EQ(result.value().descriptors.size(), 1U);
    ASSERT_EQ(result.value().diagnostics.size(), 1U);
    EXPECT_FALSE(result.value().descriptors[0].cueId().has_value());
    EXPECT_EQ(result.value().diagnostics[0].code(),
              creator::core::ErrorCode::IoFailure);
    EXPECT_NE(result.value().diagnostics[0].message().find("caption-clip"),
              std::string::npos);
    EXPECT_NE(result.value().diagnostics[0].message().find("cue-1"),
              std::string::npos);
    EXPECT_NE(result.value().diagnostics[0].message().find(
                  result.value().descriptors[0].resolvedFontFamily()),
              std::string::npos);
    EXPECT_EQ(commitAttempts, 2);
    std::size_t temporaryFiles = 0;
    std::size_t cacheEntries = 0;
    for (const auto& entry :
         fs::directory_iterator{package.path() / "cache" / "generated"}) {
        ++cacheEntries;
        if (entry.path().extension() == ".tmp") ++temporaryFiles;
    }
    EXPECT_EQ(temporaryFiles, 0U);
    EXPECT_EQ(cacheEntries, 1U);
}

#ifdef _WIN32
TEST(GeneratedOverlayCacheTest, RefusesCacheDirectoryJunctionEscape) {
    TempDirectory package;
    TempDirectory outside;
    const auto junction = package.path() / "cache";
    ASSERT_TRUE(createDirectoryJunction(junction, outside.path()));

    GeneratedOverlayCache cache;
    auto result = cache.synchronize(package.path(), generatedTimeline(), 320,
                                    180, FrameRate::create(60, 1).value());
    EXPECT_FALSE(result.hasValue());
    ASSERT_TRUE(fs::remove(junction));
}
#endif

TEST(GeneratedOverlayCacheTest, LargeRasterWorkLeavesGuiEventLoopResponsive) {
    TempDirectory package;
    std::optional<creator::core::Result<
        creator::app::GeneratedOverlayCacheResult>> result;
    std::atomic_bool finished{false};
    int timerTicks = 0;
    QTimer timer;
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] { ++timerTicks; });
    timer.start();

    std::jthread worker{[&] {
        GeneratedOverlayCache cache;
        result.emplace(cache.synchronize(
            package.path(), generatedTimeline(), 2048, 1152,
            FrameRate::create(60, 1).value()));
        finished.store(true, std::memory_order_release);
    }};
    QElapsedTimer timeout;
    timeout.start();
    while (!finished.load(std::memory_order_acquire) &&
           timeout.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    worker.join();
    timer.stop();

    ASSERT_TRUE(finished.load(std::memory_order_acquire));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hasValue()) << result->error().message();
    EXPECT_GT(timerTicks, 2);
}

}  // namespace
