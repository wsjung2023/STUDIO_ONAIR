#include "platform_release/DiagnosticBundle.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using creator::core::ErrorCode;
using creator::platform_release::DiagnosticBundle;
using creator::platform_release::DiagnosticBundleRequest;

class DiagnosticBundleTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "cs_diagnostic_bundle_test";
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_ / "source");
        std::ofstream{root_ / "source" / "application.log", std::ios::binary}
            << "startup ok\nrenderer=software\n";
        std::ofstream{root_ / "source" / "release-manifest.json", std::ios::binary}
            << R"({"schemaVersion":1,"target":"windows-x64"})";
        std::ofstream{root_ / "source" / "system-summary.json", std::ios::binary}
            << R"({"os":"Windows","memoryMiB":8192})";
        std::ofstream{root_ / "source" / "private.mp4", std::ios::binary}
            << "private recording";
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    DiagnosticBundleRequest request() const {
        return {.sourceRoot = root_ / "source",
                .destination = root_ / "diagnostics",
                .files = {"application.log", "release-manifest.json",
                          "system-summary.json"},
                .consentGranted = true};
    }

    fs::path root_;
};

TEST_F(DiagnosticBundleTest, RequiresExplicitConsentWithoutCreatingOutput) {
    auto denied = request();
    denied.consentGranted = false;

    const auto result = DiagnosticBundle::create(denied);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_FALSE(fs::exists(denied.destination));
}

TEST_F(DiagnosticBundleTest, CopiesOnlyNamedAllowlistedFilesAndRecordsHashes) {
    const auto result = DiagnosticBundle::create(request());

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value(), root_ / "diagnostics");
    EXPECT_TRUE(fs::is_regular_file(result.value() / "application.log"));
    EXPECT_TRUE(fs::is_regular_file(result.value() / "release-manifest.json"));
    EXPECT_TRUE(fs::is_regular_file(result.value() / "system-summary.json"));
    EXPECT_FALSE(fs::exists(result.value() / "private.mp4"));

    std::ifstream input{result.value() / "diagnostic-bundle-manifest.json",
                        std::ios::binary};
    const auto manifest = nlohmann::json::parse(input);
    ASSERT_EQ(manifest.at("files").size(), 3);
    for (const auto& entry : manifest.at("files")) {
        const auto path = result.value() / entry.at("name").get<std::string>();
        EXPECT_EQ(entry.at("sha256").get<std::string>(),
                  creator::core::sha256File(path).value());
    }
    for (const auto& entry : fs::directory_iterator(root_)) {
        EXPECT_FALSE(entry.path().filename().string().starts_with(".diagnostics.part-"));
    }
}

TEST_F(DiagnosticBundleTest, RejectsOutOfRootAndSymlinkInputs) {
    const auto outside = root_ / "outside.log";
    std::ofstream{outside, std::ios::binary} << "outside";
    auto escaped = request();
    escaped.files = {"../outside.log"};
    EXPECT_FALSE(DiagnosticBundle::create(escaped).hasValue());

    fs::create_directories(root_ / "source" / "linked");
    const auto link = root_ / "source" / "linked" / "application.log";
    std::error_code linkError;
    fs::create_symlink(root_ / "source" / "application.log", link, linkError);
    if (linkError) {
        linkError.clear();
        fs::create_hard_link(root_ / "source" / "application.log", link, linkError);
        ASSERT_FALSE(linkError) << "link creation is unavailable: " << linkError.message();
    }
    auto linked = request();
    linked.files = {"linked/application.log"};
    EXPECT_FALSE(DiagnosticBundle::create(linked).hasValue());
}

TEST_F(DiagnosticBundleTest, RejectsForbiddenExtensionsAndPrivateContentNames) {
    auto media = request();
    media.files = {"private.mp4"};
    EXPECT_FALSE(DiagnosticBundle::create(media).hasValue());

    std::ofstream{root_ / "source" / "application.log",
                  std::ios::binary | std::ios::trunc}
        << R"({"transcript":"customer speech"})";
    auto privateLog = request();
    privateLog.files = {"application.log"};
    const auto result = DiagnosticBundle::create(privateLog);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(privateLog.destination));

    std::ofstream{root_ / "source" / "application.log",
                  std::ios::binary | std::ios::trunc}
        << R"({"accountId":"private-account","receipt":"opaque"})";
    EXPECT_FALSE(DiagnosticBundle::create(privateLog).hasValue());
}

TEST_F(DiagnosticBundleTest, RefusesToReplaceAnExistingBundle) {
    fs::create_directories(root_ / "diagnostics");
    std::ofstream{root_ / "diagnostics" / "keep.txt"} << "keep";

    const auto result = DiagnosticBundle::create(request());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_TRUE(fs::exists(root_ / "diagnostics" / "keep.txt"));
}

}  // namespace
