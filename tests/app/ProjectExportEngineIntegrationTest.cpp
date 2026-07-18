#include "app/ProjectExportEngine.h"

#include "core/Uuid.h"
#include "domain/Timeline.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteRenderJobStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <thread>

namespace {

using namespace creator;
namespace fs = std::filesystem;

TEST(ProjectExportEngineIntegrationTest,
     PersistsAndPublishesValidatedProjectMp4EndToEnd) {
    const auto root = fs::temp_directory_path() /
                      ("creator-studio-project-export-" + core::generateUuidV4());
    const auto packageRoot = root / "acceptance.cstudio";
    fs::create_directories(root);
    struct Cleanup final {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};

    project_store::ProjectPackageStore packages;
    auto created = packages.create(packageRoot, "Export acceptance");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    const auto projectId = created.value().package.manifest.projectId;
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main",
                        core::FrameRate::create(30, 1).value())
                        .value();
    const auto trackId = domain::TrackId::create("titles").value();
    ASSERT_TRUE(timeline.addTrack(
                            domain::Track::create(trackId,
                                                  domain::TrackKind::Title,
                                                  "Titles", true, false)
                                .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(
                           core::TimestampNs{},
                           core::DurationNs{1'000'000'000})
                           .value();
    const auto title = domain::TitlePayload::create(
                           "Acceptance", "Creator Sans", 0.5, 0.5,
                           domain::RgbaColor::parse("#ffffffff").value(),
                           domain::RgbaColor::parse("#00000000").value(),
                           domain::TextAlignment::Center)
                           .value();
    ASSERT_TRUE(timeline.insertClip(
                            trackId,
                            domain::Clip::createTitle(
                                domain::ClipId::create("title").value(), range,
                                true, title, std::nullopt)
                                .value())
                    .hasValue());
    auto timelineStore = project_store::SqliteTimelineStore::open(
        created.value().databasePath, projectId);
    ASSERT_TRUE(timelineStore.hasValue()) << timelineStore.error().message();
    ASSERT_TRUE(timelineStore.value().createTimeline(timeline).hasValue());
    auto persisted = timelineStore.value().loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue()) << persisted.error().message();

    edit_engine::TimelineSnapshot snapshot{
        .timeline = persisted.value().timeline,
        .revision = domain::TimelineRevision::create(
                        persisted.value().revision)
                        .value(),
        .mediaRoot = packageRoot};
    const auto destination = root / "acceptance.mp4";
    auto request = edit_engine::RenderRequest::create(
        projectId, std::move(snapshot), destination,
        edit_engine::RenderPreset::h2641080p30().value(),
        edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(request.hasValue()) << request.error().message();
    const auto jobId = request.value().jobId();
    app::ProjectExportEngine engine{fs::path{CS_TEST_MLT_ROOT}};

    auto job = engine.render(request.value());
    ASSERT_TRUE(job.hasValue()) << job.error().message();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{30};
    edit_engine::RenderJobState state = edit_engine::RenderJobState::Pending;
    while (std::chrono::steady_clock::now() < deadline) {
        auto progress = job.value()->progress();
        ASSERT_TRUE(progress.hasValue()) << progress.error().message();
        state = progress.value().state();
        if (state == edit_engine::RenderJobState::Completed ||
            state == edit_engine::RenderJobState::Failed ||
            state == edit_engine::RenderJobState::Cancelled) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(state, edit_engine::RenderJobState::Completed);
    ASSERT_TRUE(fs::is_regular_file(destination));

    auto reopened = packages.open(packageRoot);
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    const auto lease = reopened.value().databaseIdentityLease;
    ASSERT_TRUE(lease);
    auto jobs = project_store::SqliteRenderJobStore::open(
        reopened.value().databasePath, projectId,
        [lease] { return lease->verifyCurrentIdentity(); });
    ASSERT_TRUE(jobs.hasValue()) << jobs.error().message();
    auto record = jobs.value().load(jobId);
    ASSERT_TRUE(record.hasValue()) << record.error().message();
    ASSERT_TRUE(record.value().has_value());
    EXPECT_EQ(record.value()->progress.state(),
              edit_engine::RenderJobState::Completed);
    EXPECT_TRUE(record.value()->diagnostics.selectedEncoder.has_value());
    EXPECT_TRUE(record.value()->diagnostics.outputSha256.has_value());
    EXPECT_TRUE(record.value()->diagnostics.destinationIdentity.has_value());

    ffmpeg_adapter::FfmpegMediaProbe probe;
    auto media = probe.probe(root, destination.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    EXPECT_EQ(media.value().codecName, "h264");
    ASSERT_TRUE(media.value().video.has_value());
    EXPECT_EQ(media.value().video->width, 1920);
    EXPECT_EQ(media.value().video->height, 1080);
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().audio->sampleRate, 48'000);
    EXPECT_EQ(media.value().audio->channels, 2);
}

}  // namespace
