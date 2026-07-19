#include "app/EditorController.h"
#include "app/EditorSessionWorker.h"
#include "app/GeneratedOverlayCache.h"
#include "app/TimelineTrackModel.h"

#include "core/Timebase.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "fakes/FakeEditEngine.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/JsonProjectStore.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteTimelineStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace creator;

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int32_t kCanvasWidth = 64;
constexpr std::int32_t kCanvasHeight = 36;

core::TimestampNs at(std::int64_t nanoseconds) {
  return core::TimestampNs{core::DurationNs{nanoseconds}};
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 15'000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return predicate();
}

void waitRevision(app::EditorController &controller, qlonglong revision) {
  ASSERT_TRUE(waitUntil([&] {
    return !controller.sessionBusy() && !controller.busy() &&
           controller.timelineRevision() == revision;
  })) << controller.statusMessage().toStdString();
}

void seekAndWait(app::EditorController &controller, qlonglong position) {
  controller.seek(position);
  ASSERT_TRUE(waitUntil([&] {
    return !controller.busy() && controller.playheadNs() == position;
  })) << controller.statusMessage().toStdString();
}

void writePpm(const fs::path &path, std::uint8_t red, std::uint8_t green,
              std::uint8_t blue) {
  std::ofstream output(path, std::ios::binary);
  output << "P6\n" << kCanvasWidth << ' ' << kCanvasHeight << "\n255\n";
  for (int pixel = 0; pixel < kCanvasWidth * kCanvasHeight; ++pixel) {
    output.put(static_cast<char>(red));
    output.put(static_cast<char>(green));
    output.put(static_cast<char>(blue));
  }
}

void writeMonoWave(const fs::path &path, std::int16_t sampleValue,
                   std::uint32_t sampleCount = 4U * 48'000U) {
  constexpr std::uint32_t kSampleRate = 48'000;
  constexpr std::uint16_t kChannels = 1;
  constexpr std::uint16_t kBitsPerSample = 16;
  const std::uint32_t dataBytes = sampleCount * sizeof(std::int16_t);
  std::ofstream output(path, std::ios::binary);
  const auto write16 = [&](std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
  };
  const auto write32 = [&](std::uint32_t value) {
    write16(static_cast<std::uint16_t>(value & 0xffffU));
    write16(static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
  };
  output.write("RIFF", 4);
  write32(36U + dataBytes);
  output.write("WAVEfmt ", 8);
  write32(16U);
  write16(1U);
  write16(kChannels);
  write32(kSampleRate);
  write32(kSampleRate * kChannels * (kBitsPerSample / 8U));
  write16(kChannels * (kBitsPerSample / 8U));
  write16(kBitsPerSample);
  output.write("data", 4);
  write32(dataBytes);
  for (std::uint32_t sample = 0; sample < sampleCount; ++sample) {
    write16(static_cast<std::uint16_t>(sampleValue));
  }
}

domain::MediaAsset imageAsset(std::string id, std::string path,
                              core::DurationNs duration) {
  return domain::MediaAsset::create(
             domain::AssetId::create(std::move(id)).value(),
             domain::MediaKind::Image, std::move(path), duration,
             domain::VideoAssetMetadata{kCanvasWidth, kCanvasHeight,
                                        core::FrameRate::create(30, 1).value()},
             std::nullopt, 7'000, "r1-05-image-fixture",
             domain::AssetAvailability::Available)
      .value();
}

domain::MediaAsset microphoneAsset(core::DurationNs duration) {
  return domain::MediaAsset::create(
             domain::AssetId::create("microphone").value(),
             domain::MediaKind::Audio, "media/microphone.wav", duration,
             std::nullopt, domain::AudioAssetMetadata{48'000, 1}, 384'044,
             "r1-05-audio-fixture", domain::AssetAvailability::Available)
      .value();
}

class PhysicalPackage final {
public:
  PhysicalPackage() {
    root_ = fs::temp_directory_path() /
            fs::path{u8"creator-studio-R1-05-물리검증"} /
            (std::to_string(QCoreApplication::applicationPid()) + "-" +
             std::to_string(++nextId_));
    package_ = root_ / fs::path{u8"수익형-편집.cstudio"};
    std::error_code error;
    fs::remove_all(root_, error);
    fs::create_directories(root_);

    project_store::ProjectPackageStore packages;
    auto created = packages.create(package_, "R1-05 physical acceptance");
    EXPECT_TRUE(created.hasValue())
        << (created.hasValue() ? "" : created.error().message());
    if (!created.hasValue())
      return;
    auto manifest = created.value().package.manifest;
    manifest.canvas.width = kCanvasWidth;
    manifest.canvas.height = kCanvasHeight;
    manifest.canvas.frameRateNumerator = 30;
    manifest.canvas.frameRateDenominator = 1;
    EXPECT_TRUE(
        project_store::JsonProjectStore{}.save(package_, manifest).hasValue());
    projectId_ = manifest.projectId;
    database_ = package_ / manifest.database;

    fs::create_directories(package_ / "media");
    writePpm(package_ / "media/screen-red.ppm", 240, 16, 16);
    writePpm(package_ / "media/camera-blue.ppm", 16, 32, 240);
    writeMonoWave(package_ / "media/microphone.wav", 1'000);

    const auto duration = core::DurationNs{4 * kSecond};
    auto screen = imageAsset("screen", "media/screen-red.ppm", duration);
    auto camera = imageAsset("camera", "media/camera-blue.ppm", duration);
    auto microphone = microphoneAsset(duration);
    auto timeline =
        domain::Timeline::create(domain::TimelineId::create("main").value(),
                                 "한글 물리 검증",
                                 core::FrameRate::create(30, 1).value())
            .value();
    const auto screenTrack = domain::TrackId::create("screen-1").value();
    const auto cameraTrack = domain::TrackId::create("camera-1").value();
    const auto microphoneTrack =
        domain::TrackId::create("microphone-1").value();
    EXPECT_TRUE(timeline
                    .addTrack(domain::Track::create(screenTrack,
                                                    domain::TrackKind::Video,
                                                    "화면", true, false)
                                  .value())
                    .hasValue());
    EXPECT_TRUE(timeline
                    .addTrack(domain::Track::create(cameraTrack,
                                                    domain::TrackKind::Video,
                                                    "카메라", true, false)
                                  .value())
                    .hasValue());
    EXPECT_TRUE(timeline
                    .addTrack(domain::Track::create(microphoneTrack,
                                                    domain::TrackKind::Audio,
                                                    "마이크", true, false)
                                  .value())
                    .hasValue());
    const auto range =
        domain::TimeRange::create(core::TimestampNs{}, duration).value();
    EXPECT_TRUE(
        timeline
            .insertClip(screenTrack,
                        domain::Clip::createAsset(
                            domain::ClipId::create("screen-clip").value(),
                            screen, range, range, true, std::nullopt,
                            std::nullopt)
                            .value())
            .hasValue());
    EXPECT_TRUE(
        timeline
            .insertClip(cameraTrack,
                        domain::Clip::createAsset(
                            domain::ClipId::create("camera-clip").value(),
                            camera, range, range, true, std::nullopt,
                            std::nullopt)
                            .value())
            .hasValue());
    EXPECT_TRUE(
        timeline
            .insertClip(microphoneTrack,
                        domain::Clip::createAsset(
                            domain::ClipId::create("microphone-clip").value(),
                            microphone, range, range, true, std::nullopt,
                            std::nullopt)
                            .value())
            .hasValue());

    auto opened =
        project_store::SqliteTimelineStore::open(database_, *projectId_);
    EXPECT_TRUE(opened.hasValue())
        << (opened.hasValue() ? "" : opened.error().message());
    if (!opened.hasValue())
      return;
    auto store = std::move(opened).value();
    EXPECT_TRUE(store.putAsset(screen).hasValue());
    EXPECT_TRUE(store.putAsset(camera).hasValue());
    EXPECT_TRUE(store.putAsset(microphone).hasValue());
    EXPECT_TRUE(store.createTimeline(timeline).hasValue());
  }

  ~PhysicalPackage() {
    std::error_code error;
    fs::remove_all(root_, error);
    EXPECT_FALSE(error) << "Could not remove physical package root: "
                        << error.message();
    std::error_code existsError;
    EXPECT_FALSE(fs::exists(root_, existsError))
        << "Physical package root is still locked after product teardown: "
        << root_.string();
    EXPECT_FALSE(existsError) << "Could not verify physical package cleanup: "
                              << existsError.message();
  }

  [[nodiscard]] QUrl url() const {
    return QUrl::fromLocalFile(QString::fromStdWString(package_.wstring()));
  }
  [[nodiscard]] const fs::path &packageRoot() const noexcept {
    return package_;
  }
  [[nodiscard]] const fs::path &database() const noexcept { return database_; }

  void rejectEditCommits() const {
    auto connection =
        project_store::internal::SqliteConnection::open(database_);
    ASSERT_TRUE(connection.hasValue()) << connection.error().message();
    ASSERT_TRUE(connection.value()
                    .execute("CREATE TRIGGER reject_r1_05_edit "
                             "BEFORE INSERT ON edit_commands "
                             "BEGIN SELECT RAISE(ABORT, 'r1-05 injected commit "
                             "failure'); END")
                    .hasValue());
  }

  void blockGeneratedCache() const {
    const auto generated = package_ / "cache/generated";
    std::error_code error;
    fs::remove_all(generated, error);
    fs::create_directories(generated.parent_path());
    std::ofstream blocker(generated, std::ios::binary);
    blocker << "derived-cache-blocker";
  }

  void unblockGeneratedCache() const {
    std::error_code error;
    fs::remove(package_ / "cache/generated", error);
  }

private:
  inline static std::uint64_t nextId_{0};
  fs::path root_;
  fs::path package_;
  fs::path database_;
  std::optional<domain::ProjectId> projectId_;
};

QVariantList timelineRows(app::EditorController &controller) {
  QVariantList rows;
  auto *model = controller.timelineTrackModel();
  for (int row = 0; row < model->rowCount(); ++row) {
    const auto index = model->index(row, 0);
    rows.push_back(QVariantMap{
        {QStringLiteral("trackId"),
         model->data(index, app::TimelineTrackModel::TrackIdRole)},
        {QStringLiteral("name"),
         model->data(index, app::TimelineTrackModel::NameRole)},
        {QStringLiteral("kind"),
         model->data(index, app::TimelineTrackModel::KindRole)},
        {QStringLiteral("clips"),
         model->data(index, app::TimelineTrackModel::ClipsRole)},
    });
  }
  return rows;
}

QString clipIdForKind(const QVariantList &rows, const QString &kind) {
  for (const auto &rowValue : rows) {
    for (const auto &clipValue :
         rowValue.toMap().value(QStringLiteral("clips")).toList()) {
      const auto clip = clipValue.toMap();
      if (clip.value(QStringLiteral("clipKind")).toString() == kind) {
        return clip.value(QStringLiteral("id")).toString();
      }
    }
  }
  return {};
}

std::optional<app::EditorSessionState>
loadPhysicalState(const fs::path &packageRoot) {
  app::EditorSessionWorker worker;
  app::EditorSessionResultPtr result;
  QObject::connect(
      &worker, &app::EditorSessionWorker::opened, &worker,
      [&](quint64, app::EditorSessionResultPtr value) {
        result = std::move(value);
      },
      Qt::DirectConnection);
  worker.openProject(1, packageRoot);
  if (!result || !result->hasValue())
    return std::nullopt;
  return result->value().state;
}

std::vector<std::pair<std::string, std::uintmax_t>>
fileInventory(const fs::path &root) {
  std::vector<std::pair<std::string, std::uintmax_t>> files;
  std::error_code error;
  if (!fs::exists(root, error))
    return files;
  for (fs::recursive_directory_iterator it(root, error), end;
       it != end && !error; it.increment(error)) {
    if (!it->is_regular_file(error))
      continue;
    files.emplace_back(fs::relative(it->path(), root).generic_string(),
                       it->file_size(error));
  }
  std::sort(files.begin(), files.end());
  return files;
}

double meanAbsolute(const std::vector<float> &samples) {
  double total = 0.0;
  for (const auto sample : samples)
    total += std::abs(sample);
  return samples.empty() ? 0.0 : total / static_cast<double>(samples.size());
}

struct PixelCounts final {
  std::size_t red{};
  std::size_t blue{};
  std::size_t light{};
};

PixelCounts countFramePixels(const edit_engine::PreviewFrame &frame) {
  PixelCounts counts;
  const auto &video = frame.frame();
  const auto *pixels =
      static_cast<const std::uint8_t *>(video.platformHandle.get());
  if (pixels == nullptr)
    return counts;
  for (std::uint32_t y = 0; y < video.height; ++y) {
    for (std::uint32_t x = 0; x < video.width; ++x) {
      const auto *pixel = pixels + y * video.width * 4U + x * 4U;
      const std::uint32_t blue = pixel[0];
      const std::uint32_t green = pixel[1];
      const std::uint32_t red = pixel[2];
      if (red > 150U && red > blue + 50U)
        ++counts.red;
      if (blue > 100U && blue > red + 30U)
        ++counts.blue;
      if (red > 180U && green > 180U && blue > 180U)
        ++counts.light;
    }
  }
  return counts;
}

PixelCounts countImagePixels(const QImage &image) {
  PixelCounts counts;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const auto pixel = image.pixel(x, y);
      const std::uint32_t red = qRed(pixel);
      const std::uint32_t green = qGreen(pixel);
      const std::uint32_t blue = qBlue(pixel);
      if (red > 150U && red > blue + 50U)
        ++counts.red;
      if (blue > 100U && blue > red + 30U)
        ++counts.blue;
      if (red > 180U && green > 180U && blue > 180U)
        ++counts.light;
    }
  }
  return counts;
}

#ifdef _WIN32
std::uint64_t workingSetBytes() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
          sizeof(counters))) {
    return 0;
  }
  return static_cast<std::uint64_t>(counters.WorkingSetSize);
}
#else
std::uint64_t workingSetBytes() { return 0; }
#endif

TEST(R1EffectsTextAcceptanceTest,
     ReopensExactUnicodeEffectsTextPixelsAndPcmThroughAuditedMlt) {
  PhysicalPackage package;
  QVariantList savedRows;
  QString resolvedFont;
  {
    auto engine = std::make_unique<mlt_adapter::MltEditEngine>(
        mlt_adapter::MltEditEngineConfig{.runtimeRoot =
                                             fs::path{CS_TEST_STAGED_MLT_ROOT},
                                         .previewWidth = kCanvasWidth,
                                         .previewHeight = kCanvasHeight});
    app::EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    waitRevision(controller, 0);

    controller.selectClip(QStringLiteral("camera-1"),
                          QStringLiteral("camera-clip"));
    controller.applySelectedPipPreset(QStringLiteral("topRight"));
    waitRevision(controller, 1);
    controller.applySelectedVisualTransform(0.66, 0.06, 0.28, 0.28, 1.0, 1.0,
                                            12.0, 0.04, 0.02, 0.04, 0.02, 0.72,
                                            5);
    waitRevision(controller, 2);

    controller.selectClip(QStringLiteral("microphone-1"),
                          QStringLiteral("microphone-clip"));
    controller.applySelectedAudioEnvelope(-6.0, 200'000'000, 200'000'000);
    waitRevision(controller, 3);

    seekAndWait(controller, 500'000'000);
    controller.addTitle(QString::fromUtf8("수익형 강의 제목"),
                        QStringLiteral("Noto Sans"), 0.08, 0.08,
                        QStringLiteral("#ffffffff"),
                        QStringLiteral("#00000080"), QStringLiteral("left"));
    waitRevision(controller, 4);
    controller.addCaptionCue(0, 500'000'000, QString::fromUtf8("첫 번째 자막"));
    waitRevision(controller, 5);
    controller.addCaptionCue(650'000'000, 500'000'000,
                             QString::fromUtf8("두 번째 자막"));
    waitRevision(controller, 6);

    for (qlonglong revision = 7; revision <= 10; ++revision) {
      controller.undo();
      waitRevision(controller, revision);
    }
    for (qlonglong revision = 11; revision <= 14; ++revision) {
      controller.redo();
      waitRevision(controller, revision);
    }
    controller.save();
    ASSERT_TRUE(waitUntil(
        [&] { return !controller.sessionBusy() && controller.clean(); }));

    savedRows = timelineRows(controller);
    const auto titleId = clipIdForKind(savedRows, QStringLiteral("title"));
    const auto captionId = clipIdForKind(savedRows, QStringLiteral("caption"));
    ASSERT_FALSE(titleId.isEmpty());
    ASSERT_FALSE(captionId.isEmpty());
    controller.selectClip(QStringLiteral("title-1"), titleId);
    resolvedFont = controller.selectedResolvedFontFamily();
    EXPECT_FALSE(resolvedFont.isEmpty());
    controller.selectClip(QStringLiteral("caption-1"), captionId);
    ASSERT_EQ(controller.selectedCaptionCues().size(), 2);
    EXPECT_EQ(controller.selectedCaptionCues()[0]
                  .toMap()
                  .value(QStringLiteral("text"))
                  .toString(),
              QString::fromUtf8("첫 번째 자막"));
    EXPECT_EQ(controller.selectedCaptionCues()[1]
                  .toMap()
                  .value(QStringLiteral("text"))
                  .toString(),
              QString::fromUtf8("두 번째 자막"));
  }

  auto physicalState = loadPhysicalState(package.packageRoot());
  ASSERT_TRUE(physicalState.has_value());
  EXPECT_EQ(physicalState->snapshot.revision.value(), 14);
  ASSERT_EQ(physicalState->snapshot.generatedOverlays.size(), 3U);
  for (const auto &descriptor : physicalState->snapshot.generatedOverlays) {
    EXPECT_TRUE(
        fs::is_regular_file(package.packageRoot() / descriptor.rasterPath()));
  }

  mlt_adapter::MltEditEngine physicalEngine{
      {.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
       .previewWidth = kCanvasWidth,
       .previewHeight = kCanvasHeight}};
  auto loaded = physicalEngine.load(physicalState->snapshot);
  ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
  auto frame = physicalEngine.requestFrame(at(1'200'000'000));
  ASSERT_TRUE(frame.hasValue()) << frame.error().message();
  const auto pixels = countFramePixels(frame.value());
  EXPECT_GT(pixels.red, 100U);
  EXPECT_GT(pixels.blue, 10U);
  EXPECT_GT(pixels.light, 1U);

  auto audio =
      physicalEngine.requestMixedAudio(at(1'500'000'000), 48'000, 1, 1'600);
  ASSERT_TRUE(audio.hasValue()) << audio.error().message();
  EXPECT_NEAR(meanAbsolute(audio.value()), (1'000.0 / 32'768.0) * 0.5011872336,
              0.001);
  auto diagnostics = physicalEngine.diagnostics();
  ASSERT_TRUE(diagnostics.hasValue());
  EXPECT_GE(diagnostics.value().visualBranchCount, 5U);
  EXPECT_GE(diagnostics.value().transformedVisualBranchCount, 1U);
  EXPECT_EQ(diagnostics.value().audioEnvelopeBranchCount, 1U);
  EXPECT_EQ(diagnostics.value().missingOverlayCount, 0U);

  auto reopenedEngine = std::make_unique<mlt_adapter::MltEditEngine>(
      mlt_adapter::MltEditEngineConfig{.runtimeRoot =
                                           fs::path{CS_TEST_STAGED_MLT_ROOT},
                                       .previewWidth = kCanvasWidth,
                                       .previewHeight = kCanvasHeight});
  app::EditorController reopened{std::move(reopenedEngine)};
  reopened.openProject(package.url());
  waitRevision(reopened, 14);
  EXPECT_EQ(timelineRows(reopened), savedRows);
  EXPECT_TRUE(reopened.clean());
  const auto reopenedTitleId =
      clipIdForKind(savedRows, QStringLiteral("title"));
  reopened.selectClip(QStringLiteral("title-1"), reopenedTitleId);
  EXPECT_EQ(reopened.selectedResolvedFontFamily(), resolvedFont);
}

TEST(R1EffectsTextAcceptanceTest,
     RepeatedSqliteFailureChangesNoRevisionModelCacheOrEngine) {
  for (int repetition = 0; repetition < 2; ++repetition) {
    PhysicalPackage package;
    auto engine = std::make_unique<fakes::FakeEditEngine>();
    auto *fake = engine.get();
    app::EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    waitRevision(controller, 0);
    controller.selectClip(QStringLiteral("camera-1"),
                          QStringLiteral("camera-clip"));
    const auto rowsBefore = timelineRows(controller);
    const auto callsBefore = fake->calls();
    const auto cacheBefore = fileInventory(package.packageRoot() / "cache");
    package.rejectEditCommits();

    controller.applySelectedPipPreset(QStringLiteral("topLeft"));
    ASSERT_TRUE(waitUntil([&] { return !controller.sessionBusy(); }));
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_EQ(timelineRows(controller), rowsBefore);
    EXPECT_EQ(fake->calls(), callsBefore);
    EXPECT_EQ(fileInventory(package.packageRoot() / "cache"), cacheBefore);
    EXPECT_TRUE(
        controller.statusMessage().contains(QStringLiteral("commit failure")));
  }
}

TEST(R1EffectsTextAcceptanceTest,
     RepeatedCacheFailureCommitsThenRegeneratesAfterFullReopen) {
  for (int repetition = 0; repetition < 2; ++repetition) {
    PhysicalPackage package;
    {
      auto engine = std::make_unique<fakes::FakeEditEngine>();
      app::EditorController controller{std::move(engine)};
      controller.openProject(package.url());
      waitRevision(controller, 0);
      seekAndWait(controller, 250'000'000);
      package.blockGeneratedCache();
      controller.addTitle(
          QString::fromUtf8("캐시 실패 후 복구"), QStringLiteral("Noto Sans"),
          0.1, 0.1, QStringLiteral("#ffffffff"), QStringLiteral("#00000080"),
          QStringLiteral("center"));
      waitRevision(controller, 1);
      EXPECT_TRUE(controller.previewStale());
      EXPECT_FALSE(controller.statusMessage().isEmpty());
      EXPECT_FALSE(
          clipIdForKind(timelineRows(controller), QStringLiteral("title"))
              .isEmpty());
    }

    package.unblockGeneratedCache();
    auto recovered = loadPhysicalState(package.packageRoot());
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered->snapshot.revision.value(), 1);
    ASSERT_EQ(recovered->snapshot.generatedOverlays.size(), 1U);
    EXPECT_TRUE(fs::is_regular_file(
        package.packageRoot() /
        recovered->snapshot.generatedOverlays[0].rasterPath()));

    auto engine = std::make_unique<fakes::FakeEditEngine>();
    app::EditorController reopened{std::move(engine)};
    reopened.openProject(package.url());
    waitRevision(reopened, 1);
    EXPECT_FALSE(reopened.previewStale());
    EXPECT_FALSE(clipIdForKind(timelineRows(reopened), QStringLiteral("title"))
                     .isEmpty());
  }
}

edit_engine::TimelineSnapshot longFormSnapshot(const PhysicalPackage &package) {
  constexpr std::int64_t kDuration = 30 * 60 * kSecond;
  const auto duration = core::DurationNs{kDuration};
  auto screen = imageAsset("long-screen", "media/screen-red.ppm", duration);
  auto camera = imageAsset("long-camera", "media/camera-blue.ppm", duration);
  auto microphone = microphoneAsset(duration);
  auto timeline =
      domain::Timeline::create(domain::TimelineId::create("long-form").value(),
                               "30-minute representative",
                               core::FrameRate::create(30, 1).value())
          .value();
  const auto screenTrack = domain::TrackId::create("screen").value();
  const auto cameraTrack = domain::TrackId::create("camera").value();
  const auto audioTrack = domain::TrackId::create("audio").value();
  const auto titleTrack = domain::TrackId::create("titles").value();
  const auto captionTrack = domain::TrackId::create("captions").value();
  EXPECT_TRUE(
      timeline
          .addTrack(domain::Track::create(screenTrack, domain::TrackKind::Video,
                                          "Screen", true, false)
                        .value())
          .hasValue());
  EXPECT_TRUE(
      timeline
          .addTrack(domain::Track::create(cameraTrack, domain::TrackKind::Video,
                                          "Camera", true, false)
                        .value())
          .hasValue());
  EXPECT_TRUE(
      timeline
          .addTrack(domain::Track::create(audioTrack, domain::TrackKind::Audio,
                                          "Audio", true, false)
                        .value())
          .hasValue());
  EXPECT_TRUE(
      timeline
          .addTrack(domain::Track::create(titleTrack, domain::TrackKind::Title,
                                          "Titles", true, false)
                        .value())
          .hasValue());
  EXPECT_TRUE(timeline
                  .addTrack(domain::Track::create(captionTrack,
                                                  domain::TrackKind::Caption,
                                                  "Captions", true, false)
                                .value())
                  .hasValue());
  const auto fullRange =
      domain::TimeRange::create(core::TimestampNs{}, duration).value();
  const auto pip =
      domain::VisualTransform::create(0.66, 0.06, 0.28, 0.28, 1.0, 1.0, 8.0,
                                      0.02, 0.02, 0.02, 0.02, 0.8, 5)
          .value();
  const auto envelope =
      domain::AudioEnvelope::create(-3.0, core::DurationNs{500'000'000},
                                    core::DurationNs{500'000'000}, duration)
          .value();
  EXPECT_TRUE(timeline
                  .insertClip(screenTrack,
                              domain::Clip::createAsset(
                                  domain::ClipId::create("screen-long").value(),
                                  screen, fullRange, fullRange, true,
                                  std::nullopt, std::nullopt)
                                  .value())
                  .hasValue());
  EXPECT_TRUE(
      timeline
          .insertClip(cameraTrack,
                      domain::Clip::createAsset(
                          domain::ClipId::create("camera-long").value(), camera,
                          fullRange, fullRange, true, pip, std::nullopt)
                          .value())
          .hasValue());
  EXPECT_TRUE(timeline
                  .insertClip(audioTrack,
                              domain::Clip::createAsset(
                                  domain::ClipId::create("audio-long").value(),
                                  microphone, fullRange, fullRange, true,
                                  std::nullopt, envelope)
                                  .value())
                  .hasValue());

  for (int index = 0; index < 10; ++index) {
    const auto range = domain::TimeRange::create(
                           at(static_cast<std::int64_t>(index) * 180 * kSecond),
                           core::DurationNs{3 * kSecond})
                           .value();
    const auto title =
        domain::TitlePayload::create(
            "30분 강의 제목 " + std::to_string(index), "Noto Sans", 0.08, 0.08,
            domain::RgbaColor::parse("#ffffffff").value(),
            domain::RgbaColor::parse("#00000080").value(),
            domain::TextAlignment::Left)
            .value();
    EXPECT_TRUE(
        timeline
            .insertClip(titleTrack, domain::Clip::createTitle(
                                        domain::ClipId::create(
                                            "title-" + std::to_string(index))
                                            .value(),
                                        range, true, title, std::nullopt)
                                        .value())
            .hasValue());
  }
  std::vector<domain::CaptionCue> cues;
  cues.reserve(60);
  for (int index = 0; index < 60; ++index) {
    cues.push_back(
        domain::CaptionCue::create(
            domain::CueId::create("cue-" + std::to_string(index)).value(),
            core::DurationNs{static_cast<std::int64_t>(index) * 30 * kSecond},
            core::DurationNs{2 * kSecond}, "30분 자막 " + std::to_string(index))
            .value());
  }
  EXPECT_TRUE(
      timeline
          .insertClip(captionTrack,
                      domain::Clip::createCaption(
                          domain::ClipId::create("caption-long").value(),
                          fullRange, true, std::move(cues), std::nullopt)
                          .value())
          .hasValue());

  app::GeneratedOverlayCache cache;
  auto generated =
      cache.synchronize(package.packageRoot(), timeline, kCanvasWidth,
                        kCanvasHeight, core::FrameRate::create(30, 1).value());
  EXPECT_TRUE(generated.hasValue())
      << (generated.hasValue() ? "" : generated.error().message());
  std::vector<edit_engine::GeneratedOverlayDescriptor> descriptors;
  if (generated.hasValue()) {
    descriptors = std::move(generated).value().descriptors;
  }
  return edit_engine::TimelineSnapshot{
      std::move(timeline),
      domain::TimelineRevision::create(1).value(),
      {std::move(screen), std::move(camera), std::move(microphone)},
      package.packageRoot(),
      kCanvasWidth,
      kCanvasHeight,
      std::move(descriptors)};
}

struct LongGraphMetrics final {
  qint64 loadMs{};
  qint64 frameMs{};
  mlt_adapter::MltEditEngineDiagnostics diagnostics{};
  PixelCounts pixels{};
  std::string error;
};

struct LongGraphProbeState final {
  std::mutex mutex;
  qint64 loadMs{};
  qint64 frameMs{};
  std::size_t frameCalls{};
  mlt_adapter::MltEditEngineDiagnostics diagnostics{};
  std::string error;
};

class ProbedMltEditEngine final : public edit_engine::IEditEngine {
public:
  explicit ProbedMltEditEngine(std::shared_ptr<LongGraphProbeState> state)
      : state_(std::move(state)),
        engine_({.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
                 .previewWidth = kCanvasWidth,
                 .previewHeight = kCanvasHeight}) {}

  core::Result<void>
  load(const edit_engine::TimelineSnapshot &snapshot) override {
    QElapsedTimer timer;
    timer.start();
    auto result = engine_.load(snapshot);
    std::lock_guard lock{state_->mutex};
    state_->loadMs = timer.elapsed();
    if (!result.hasValue())
      state_->error = result.error().message();
    return result;
  }

  core::Result<void>
  update(const edit_engine::TimelineChangeSet &change) override {
    return engine_.update(change);
  }

  core::Result<void> play() override { return engine_.play(); }
  core::Result<void> pause() override { return engine_.pause(); }

  core::Result<void> seek(core::TimestampNs position) override {
    return engine_.seek(position);
  }

  core::Result<edit_engine::PreviewFrame>
  requestFrame(core::TimestampNs position) override {
    QElapsedTimer timer;
    timer.start();
    auto result = engine_.requestFrame(position);
    const auto elapsed = timer.elapsed();
    auto diagnostics = engine_.diagnostics();
    std::lock_guard lock{state_->mutex};
    state_->frameMs = elapsed;
    ++state_->frameCalls;
    if (!result.hasValue()) {
      state_->error = result.error().message();
    } else if (!diagnostics.hasValue()) {
      state_->error = diagnostics.error().message();
    } else {
      state_->diagnostics = diagnostics.value();
    }
    return result;
  }

  core::Result<std::unique_ptr<edit_engine::IRenderJob>>
  render(const edit_engine::RenderRequest &request) override {
    return engine_.render(request);
  }

private:
  std::shared_ptr<LongGraphProbeState> state_;
  mlt_adapter::MltEditEngine engine_;
};

TEST(R1EffectsTextAcceptanceTest,
     ThirtyMinuteGraphStaysBoundedMeasuredAndUiResponsive) {
  PhysicalPackage package;
  auto snapshot = longFormSnapshot(package);
  ASSERT_EQ(snapshot.generatedOverlays.size(), 70U);
  const auto memoryBefore = workingSetBytes();
  auto probe = std::make_shared<LongGraphProbeState>();
  auto engine = std::make_unique<ProbedMltEditEngine>(probe);
  app::EditorController controller{std::move(engine)};
  std::size_t eventLoopPasses = 0;
  qint64 maximumUiGapMs = 0;
  std::uint64_t peakWorkingSet = memoryBefore;
  const auto frameCalls = [&] {
    std::lock_guard lock{probe->mutex};
    return probe->frameCalls;
  };
  const auto pumpUntilResponsive = [&](const std::function<bool()> &predicate,
                                       int timeoutMs) {
    QElapsedTimer deadline;
    QElapsedTimer uiGap;
    deadline.start();
    uiGap.start();
    while (!predicate() && deadline.elapsed() < timeoutMs) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      maximumUiGapMs = std::max(maximumUiGapMs, uiGap.restart());
      ++eventLoopPasses;
      peakWorkingSet = std::max(peakWorkingSet, workingSetBytes());
      QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    maximumUiGapMs = std::max(maximumUiGapMs, uiGap.restart());
    return predicate();
  };

  auto assets = snapshot.assets;
  QElapsedTimer openCall;
  openCall.start();
  controller.openSession(std::move(assets), std::move(snapshot));
  maximumUiGapMs = std::max(maximumUiGapMs, openCall.elapsed());
  ASSERT_TRUE(pumpUntilResponsive(
      [&] {
        return !controller.busy() && controller.hasPreviewFrame() &&
               frameCalls() >= 1U;
      },
      15'000))
      << controller.statusMessage().toStdString();

  const auto callsBeforeSeek = frameCalls();
  constexpr qlonglong kMiddlePosition = 15 * 60 * kSecond;
  QElapsedTimer seekCall;
  seekCall.start();
  controller.seek(kMiddlePosition);
  maximumUiGapMs = std::max(maximumUiGapMs, seekCall.elapsed());
  ASSERT_TRUE(pumpUntilResponsive(
      [&] {
        return !controller.busy() && controller.hasPreviewFrame() &&
               controller.playheadNs() == kMiddlePosition &&
               frameCalls() > callsBeforeSeek;
      },
      15'000))
      << controller.statusMessage().toStdString();

  LongGraphMetrics metrics;
  {
    std::lock_guard lock{probe->mutex};
    metrics.loadMs = probe->loadMs;
    metrics.frameMs = probe->frameMs;
    metrics.diagnostics = probe->diagnostics;
    metrics.error = probe->error;
  }
  metrics.pixels = countImagePixels(controller.previewImage());
  peakWorkingSet = std::max(peakWorkingSet, workingSetBytes());
  const auto workingSetDelta =
      peakWorkingSet >= memoryBefore ? peakWorkingSet - memoryBefore : 0;

  std::cout << "[ R1-05 METRICS ] graph_build_ms=" << metrics.loadMs
            << " frame_request_ms=" << metrics.frameMs
            << " visual_branches=" << metrics.diagnostics.visualBranchCount
            << " composite_transitions="
            << metrics.diagnostics.videoCompositeTransitions
            << " audio_mix_transitions="
            << metrics.diagnostics.audioMixTransitions
            << " event_loop_passes=" << eventLoopPasses
            << " max_ui_gap_ms=" << maximumUiGapMs
            << " working_set_delta_bytes=" << workingSetDelta << '\n';

  ASSERT_TRUE(metrics.error.empty()) << metrics.error;
  EXPECT_GT(eventLoopPasses, 0U);
  EXPECT_LT(maximumUiGapMs, 250);
  EXPECT_LT(metrics.loadMs, 10'000);
  EXPECT_LT(metrics.frameMs, 3'000);
  EXPECT_EQ(metrics.diagnostics.visualBranchCount, 72U);
  EXPECT_EQ(metrics.diagnostics.transformedVisualBranchCount, 1U);
  EXPECT_EQ(metrics.diagnostics.audioEnvelopeBranchCount, 1U);
  EXPECT_EQ(metrics.diagnostics.missingOverlayCount, 0U);
  EXPECT_GT(metrics.pixels.red, 100U);
#ifdef _WIN32
  EXPECT_LT(workingSetDelta, 768ULL * 1024ULL * 1024ULL);
#endif
}

} // namespace

int main(int argc, char **argv) {
  QGuiApplication app{argc, argv};
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
