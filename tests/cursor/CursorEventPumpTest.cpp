#include "cursor/CursorEventPump.h"

#include "core/Timebase.h"
#include "cursor/CursorNdjsonSink.h"
#include "fakes/FakeCursorSource.h"

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
using creator::cursor::CursorEventPump;
using creator::cursor::RawCursorClickSample;
using creator::cursor::RawCursorMoveSample;
using creator::fakes::FakeCursorSource;
using creator::domain::SourceId;

TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

class CursorEventPumpTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("cs_cursor_pump_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        path_ = dir_ / "cursor.ndjson";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::vector<nlohmann::json> readLines() const {
        std::vector<nlohmann::json> result;
        std::ifstream in{path_, std::ios::binary};
        std::string line;
        while (std::getline(in, line)) result.push_back(nlohmann::json::parse(line));
        return result;
    }

    std::filesystem::path dir_;
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

TEST_F(CursorEventPumpTest, DrainsRawSamplesIntoSchemaValidNdjsonInOrder) {
    const auto sourceId = SourceId::create("screen-1").value();
    auto sink = creator::cursor::CursorNdjsonSink::create(path_);
    ASSERT_TRUE(sink.hasValue());
    auto source = std::make_unique<FakeCursorSource>(std::vector<creator::cursor::RawCursorSample>{
        RawCursorMoveSample{at(10), 0, 0, 1920, 1080},
        RawCursorClickSample{at(20), 960, 540, 1920, 1080, 0},
        RawCursorMoveSample{at(30), 1920, 1080, 1920, 1080},
    });
    auto pump = CursorEventPump::create(std::move(source), std::move(sink).value(), sourceId);
    ASSERT_TRUE(pump.hasValue());

    ASSERT_TRUE(pump.value()->drain(2).hasValue());
    EXPECT_EQ(pump.value()->stats().polled, 2U);
    EXPECT_EQ(pump.value()->stats().moves, 1U);
    EXPECT_EQ(pump.value()->stats().clicks, 1U);
    ASSERT_TRUE(pump.value()->drain(8).hasValue());
    EXPECT_EQ(pump.value()->stats().polled, 3U);

    const auto lines = readLines();
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(cursor_test::validateEvent(lines[0]), "");
    EXPECT_EQ(cursor_test::validateEvent(lines[1]), "");
    EXPECT_EQ(cursor_test::validateEvent(lines[2]), "");
    EXPECT_EQ(lines[0]["type"], "cursor.move");
    EXPECT_EQ(lines[1]["type"], "cursor.click");
    EXPECT_EQ(lines[2]["type"], "cursor.move");
}

TEST_F(CursorEventPumpTest, RejectsUnknownButtonWithoutWritingPartialEvent) {
    const auto sourceId = SourceId::create("screen-1").value();
    auto sink = creator::cursor::CursorNdjsonSink::create(path_);
    ASSERT_TRUE(sink.hasValue());
    auto source = std::make_unique<FakeCursorSource>(std::vector<creator::cursor::RawCursorSample>{
        RawCursorClickSample{at(10), 10, 10, 100, 100, 9},
    });
    auto pump = CursorEventPump::create(std::move(source), std::move(sink).value(), sourceId);
    ASSERT_TRUE(pump.hasValue());

    const auto result = pump.value()->drain(1);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(readLines().empty());
    EXPECT_EQ(pump.value()->stats().invalid, 1U);
}

}  // namespace

