#include "cursor/CursorNdjsonSink.h"

#include "cursor/CursorClickEvent.h"
#include "cursor/CursorMoveEvent.h"
#include "cursor/CursorNormalizer.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"

#include "CursorSchemaValidator.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::cursor::CursorButton;
using creator::cursor::CursorClickEvent;
using creator::cursor::CursorMoveEvent;
using creator::cursor::CursorNdjsonSink;
using creator::cursor::CursorNormalizer;
using creator::domain::SourceId;

TimestampNs at(std::int64_t ns) {
    return TimestampNs{DurationNs{ns}};
}

// Temp-dir fixture: creates a unique directory for the telemetry file and
// removes it in TearDown so no artifact leaks between runs (CLAUDE.md §8
// resource-cleanup requirement).
class CursorNdjsonSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cs_cursor_ndjson_" +
                std::to_string(
                    ::testing::UnitTest::GetInstance()->current_test_info()->line()) +
                "_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "cursor.ndjson";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::vector<std::string> readLines() const {
        std::vector<std::string> lines;
        std::ifstream in{path_, std::ios::binary};
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    std::filesystem::path dir_;
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

TEST_F(CursorNdjsonSinkTest, WritesOneValidatingLinePerEvent) {
    const auto sourceId = SourceId::create("screen-1").value();
    const auto point = CursorNormalizer::normalize(960, 540, 1920, 1080).value();

    {
        auto sink = CursorNdjsonSink::create(path_);
        ASSERT_TRUE(sink.hasValue());
        auto& s = *sink.value();

        for (int i = 0; i < 3; ++i) {
            const auto move =
                CursorMoveEvent::create(at(i * 1'000), point, sourceId).value();
            EXPECT_TRUE(s.write(move).hasValue());
        }
        const auto click =
            CursorClickEvent::create(at(9'000), point, CursorButton::Left).value();
        EXPECT_TRUE(s.write(click).hasValue());
    }  // RAII: sink destroyed, file closed here.

    const auto lines = readLines();
    ASSERT_EQ(lines.size(), 4u);
    for (const auto& line : lines) {
        const auto json = nlohmann::json::parse(line);
        EXPECT_EQ(cursor_test::validateEvent(json), "") << line;
    }
    // Ordered: the last line is the click, the earlier ones are moves.
    EXPECT_EQ(nlohmann::json::parse(lines[0])["type"], "cursor.move");
    EXPECT_EQ(nlohmann::json::parse(lines[3])["type"], "cursor.click");
}

TEST_F(CursorNdjsonSinkTest, CreateFailsForUnopenablePath) {
    // A path whose parent directory does not exist cannot be opened.
    const auto bad = dir_ / "missing-subdir" / "cursor.ndjson";
    const auto sink = CursorNdjsonSink::create(bad);
    ASSERT_FALSE(sink.hasValue());
    EXPECT_EQ(sink.error().code(), creator::core::ErrorCode::IoFailure);
}

}  // namespace
