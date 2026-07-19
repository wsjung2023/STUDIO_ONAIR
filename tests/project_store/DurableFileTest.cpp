#include "project_store/internal/DurableFile.h"

#include "core/Uuid.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;

TEST(DurableFileTest, AtomicallyWritesAWindowsSafeLongTargetPath) {
    const auto root = fs::temp_directory_path() /
                      ("cs_durable_" + creator::core::generateUuidV4());
    const std::string filename =
        "derived-concat-" + std::string(120, 'a') + ".ffconcat";
    constexpr std::size_t targetLength = 240;
    ASSERT_LT(root.native().size() + filename.size() + 2U, targetLength);
    const auto paddingLength =
        targetLength - root.native().size() - filename.size() - 2U;
    const auto directory = root / std::string(paddingLength, 'd');
    const auto target = directory / filename;
    const auto legacyTemporary = directory /
                                 ("." + filename + ".part-" +
                                  creator::core::generateUuidV4());
    ASSERT_LE(target.native().size(), targetLength);
    ASSERT_GT(legacyTemporary.native().size(), 260U);
    ASSERT_TRUE(fs::create_directories(directory));

    const auto written = creator::project_store::internal::writeFileDurably(
        target, "ffconcat version 1.0\n");

    ASSERT_TRUE(written.hasValue()) << written.error().message();
    std::ifstream input{target, std::ios::binary};
    ASSERT_TRUE(input);
    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_EQ(contents, "ffconcat version 1.0\n");

    std::error_code ignored;
    fs::remove_all(root, ignored);
}

}  // namespace
