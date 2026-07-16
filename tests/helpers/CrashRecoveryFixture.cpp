#include "project_store/ProjectPackageStore.h"

#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "domain/Segment.h"
#include "project_store/SqliteProjectDatabase.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

int run(const fs::path& packagePath) {
    using creator::core::TimestampNs;
    using creator::core::Utc;
    using creator::domain::SegmentInfo;
    using creator::domain::SegmentStatus;
    using creator::domain::SessionId;
    using creator::domain::SourceId;
    using creator::project_store::ProjectPackageStore;
    using creator::project_store::SqliteProjectDatabase;

    ProjectPackageStore store;
    auto created = store.create(packagePath, "Crash Fixture");
    if (!created.hasValue()) return 10;
    auto database = SqliteProjectDatabase::open(
        packagePath / created.value().package.manifest.database,
        created.value().package.manifest.projectId);
    if (!database.hasValue()) return 11;
    const auto session = SessionId::create("session-crash").value();
    if (!database.value()
             .beginRecording(session, TimestampNs{}, Utc::now())
             .hasValue()) {
        return 12;
    }
    const auto source = SourceId::create("screen-1").value();
    const SegmentInfo readyWriting{
        .index = 0,
        .sourceId = source,
        .startTime = TimestampNs{},
        .duration = std::chrono::seconds{2},
        .status = SegmentStatus::Writing,
        .relativePath = "media/screen-1/ready.mkv",
    };
    if (!database.value().beginSegment(session, readyWriting).hasValue()) return 13;
    SegmentInfo ready = readyWriting;
    ready.status = SegmentStatus::Ready;
    if (!database.value().markSegmentReady(session, ready).hasValue()) return 14;
    const SegmentInfo writing{
        .index = 1,
        .sourceId = source,
        .startTime = TimestampNs{} + std::chrono::seconds{2},
        .duration = std::chrono::seconds{2},
        .status = SegmentStatus::Writing,
        .relativePath = "media/screen-1/writing.mkv",
    };
    if (!database.value().beginSegment(session, writing).hasValue()) return 15;

    std::error_code ec;
    fs::create_directories(packagePath / "media" / "screen-1", ec);
    if (ec) return 16;
    std::ofstream readyFile{packagePath / ready.relativePath, std::ios::binary};
    readyFile << "ready-fixture-bytes";
    readyFile.flush();
    if (!readyFile.good()) return 17;
    readyFile.close();
    std::ofstream writingFile{packagePath / writing.relativePath, std::ios::binary};
    writingFile << "writing-fixture-bytes";
    writingFile.flush();
    if (!writingFile.good()) return 18;
    writingFile.close();

    // database remains alive. _Exit bypasses every C++ destructor, matching an
    // abrupt process death while committed WAL state and media files exist.
    std::_Exit(73);
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) return 2;
    return run(std::filesystem::path{argv[1]});
}
#else
int main(int argc, char* argv[]) {
    if (argc != 2) return 2;
    return run(std::filesystem::path{argv[1]});
}
#endif
