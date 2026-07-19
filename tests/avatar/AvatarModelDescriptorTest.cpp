#include "avatar/AvatarModelDescriptor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

using creator::avatar::AvatarModelDescriptor;
using creator::core::ErrorCode;

class AvatarModelDescriptorTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "creator-studio-avatar-descriptor";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "models");
        std::ofstream{root_ / "models" / "avatar.inx"} << "model";
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    void write(std::string text) { std::ofstream{root_ / "avatar.json"} << text; }
    std::filesystem::path root_;
};

TEST_F(AvatarModelDescriptorTest, LoadsRelativeModelAndParameterMapping) {
    write(R"({
      "schemaVersion": 1,
      "renderer": "inochi2d",
      "model": "models/avatar.inx",
      "canvas": {"width": 800, "height": 600},
      "parameters": [{"name": "ParamMouth", "source": "mouthOpen",
                       "minimum": 0.1, "maximum": 0.9}]
    })");
    const auto result = AvatarModelDescriptor::load(root_ / "avatar.json");
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().renderer(), "inochi2d");
    EXPECT_EQ(result.value().canvasWidth(), 800U);
    EXPECT_EQ(result.value().canvasHeight(), 600U);
    EXPECT_EQ(result.value().modelPath(), root_ / "models" / "avatar.inx");
    ASSERT_EQ(result.value().parameterMapper().bindings().size(), 1U);
    EXPECT_EQ(result.value().parameterMapper().bindings()[0].modelParameter,
              "ParamMouth");
}

TEST_F(AvatarModelDescriptorTest, RejectsTraversalAndMissingModel) {
    write(R"({"schemaVersion":1,"renderer":"inochi2d","model":"../avatar.inx",
              "canvas":{"width":1,"height":1}})");
    auto result = AvatarModelDescriptor::load(root_ / "avatar.json");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);

    write(R"({"schemaVersion":1,"renderer":"inochi2d","model":"models/missing.inx",
              "canvas":{"width":1,"height":1}})");
    result = AvatarModelDescriptor::load(root_ / "avatar.json");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST_F(AvatarModelDescriptorTest, RejectsMalformedAndUnknownParameterSource) {
    write("not-json");
    auto malformed = AvatarModelDescriptor::load(root_ / "avatar.json");
    ASSERT_FALSE(malformed.hasValue());
    EXPECT_EQ(malformed.error().code(), ErrorCode::ParseFailure);

    write(R"({"schemaVersion":1,"renderer":"inochi2d","model":"models/avatar.inx",
              "canvas":{"width":1,"height":1},
              "parameters":[{"name":"x","source":"unknown"}]})");
    const auto unknown = AvatarModelDescriptor::load(root_ / "avatar.json");
    ASSERT_FALSE(unknown.hasValue());
    EXPECT_EQ(unknown.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
