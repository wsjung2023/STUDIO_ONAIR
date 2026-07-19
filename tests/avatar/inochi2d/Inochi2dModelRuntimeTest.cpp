#include "avatar/inochi2d/Inochi2dModelRuntime.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

using creator::avatar::inochi2d::Inochi2dModelRuntime;
using creator::core::ErrorCode;

TEST(Inochi2dModelRuntimeTest, ReportsMissingExternalSdkWithoutAProcessCrash) {
    const auto result = Inochi2dModelRuntime::open(
        std::filesystem::temp_directory_path() / "missing-inochi2d.dll",
        std::filesystem::temp_directory_path() / "missing-avatar.inx");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(Inochi2dModelRuntimeTest, ReportsMissingModelBeforeLoadingRuntime) {
    const auto runtime = std::filesystem::temp_directory_path() /
#ifdef _WIN32
        "kernel32.dll";
#else
        "libc.so.6";
#endif
    const auto result = Inochi2dModelRuntime::open(
        runtime, std::filesystem::temp_directory_path() / "missing-avatar.inx");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(Inochi2dModelRuntimeTest, ReportsUnsupportedExternalSdkAfterModelValidation) {
    const auto runtime = std::filesystem::path{
#ifdef _WIN32
        L"C:\\Windows"
#else
        "/usr/lib"
#endif
    };
#ifdef _WIN32
    const auto library = runtime / "System32" / "kernel32.dll";
#else
    const auto library = std::filesystem::path{"/lib/x86_64-linux-gnu/libc.so.6"};
#endif
    const auto model = std::filesystem::temp_directory_path() /
                       "creator-studio-inochi2d-invalid-sdk.inx";
    { std::ofstream output(model, std::ios::binary); output << "not-an-inx"; }
    const auto result = Inochi2dModelRuntime::open(library, model);
    std::error_code ignored;
    std::filesystem::remove(model, ignored);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

}  // namespace
