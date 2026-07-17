#include "core/Sha256.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace {

namespace fs = std::filesystem;
using creator::core::Sha256;

std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST(Sha256Test, MatchesPublishedEmptyAndAbcVectors) {
    Sha256 empty;
    EXPECT_EQ(empty.finish(),
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855");

    Sha256 abc;
    abc.update(bytes("a"));
    abc.update(bytes("bc"));
    EXPECT_EQ(abc.finish(),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, StreamsFileAndRejectsMissingInput) {
    const auto path = fs::temp_directory_path() / "creator-studio-sha256-vector.bin";
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output << "abc";
    }
    const auto hash = creator::core::sha256File(path);
    ASSERT_TRUE(hash.hasValue()) << hash.error().message();
    EXPECT_EQ(hash.value(),
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad");
    std::error_code ignored;
    fs::remove(path, ignored);

    const auto missing = creator::core::sha256File(path);
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error().code(), creator::core::ErrorCode::IoFailure);
}

}  // namespace
