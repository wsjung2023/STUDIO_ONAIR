#include "avatar/inochi2d/Inochi2dRuntimeManifest.h"

#include "core/Sha256.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::avatar::inochi2d::Inochi2dRuntimeManifest;
using creator::core::ErrorCode;

constexpr std::array<std::string_view, 16> kRequiredSymbols{
    "in_puppet_load",
    "in_puppet_free",
    "in_puppet_get_parameters",
    "in_parameter_get_name",
    "in_parameter_get_dimensions",
    "in_parameter_set_value",
    "in_puppet_update",
    "in_puppet_draw",
    "in_puppet_get_drawlist",
    "in_drawlist_get_commands",
    "in_drawlist_get_vertex_data",
    "in_drawlist_get_index_data",
    "in_texture_get_width",
    "in_texture_get_height",
    "in_texture_get_channels",
    "in_texture_get_pixels",
};

constexpr std::string_view kLicense =
    "BSD 2-Clause License\n"
    "\n"
    "Copyright (c) 2020, Inochi2D Project\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are met:\n"
    "\n"
    "1. Redistributions of source code must retain the above copyright notice, "
    "this\n"
    "   list of conditions and the following disclaimer.\n"
    "\n"
    "2. Redistributions in binary form must reproduce the above copyright "
    "notice,\n"
    "   this list of conditions and the following disclaimer in the documentation\n"
    "   and/or other materials provided with the distribution.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
    "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
    "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE "
    "ARE\n"
    "DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE\n"
    "FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL\n"
    "DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR\n"
    "SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER\n"
    "CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,\n"
    "OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n";

struct PlatformIdentity final {
    std::string_view target;
    std::string_view triple;
    std::string_view minimumPlatform;
    std::string_view libraryPath;
};

constexpr PlatformIdentity currentPlatform() {
#if defined(_WIN32) && defined(_M_X64)
    return {"windows-x64", "x86_64-pc-windows-msvc", "windows-10-1809",
            "bin/inochi2d.dll"};
#elif defined(__APPLE__) && defined(__aarch64__)
    return {"macos-arm64", "arm64-apple-darwin", "macos-13.0",
            "lib/libinochi2d.dylib"};
#elif defined(__ANDROID__) && defined(__aarch64__)
    return {"android-arm64", "aarch64-linux-android26", "android-api-26",
            "lib/libinochi2d.so"};
#else
    return {};
#endif
}

class RuntimeFixture final {
public:
    RuntimeFixture() {
        root_ = fs::temp_directory_path() /
                ("creator-studio-inochi2d-manifest-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_);
    }

    ~RuntimeFixture() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void writeValidRuntime(
        std::string_view abiMode = "IN_VEC2_POSITION",
        std::string_view targetOverride = {},
        std::string_view omittedSymbol = {},
        bool matchingArchitecture = true) {
        const auto platform = currentPlatform();
        ASSERT_FALSE(platform.target.empty());
        const auto library = root_ / fs::path{platform.libraryPath};
        fs::create_directories(library.parent_path());
        writeLibraryHeader(library, matchingArchitecture);
        {
            std::ofstream notice(root_ / "LICENSE",
                                 std::ios::binary | std::ios::trunc);
            notice << kLicense;
        }
        {
            std::ofstream notices(root_ / "THIRD_PARTY_NOTICES.txt",
                                  std::ios::binary | std::ios::trunc);
            notices << "verified dependency notices fixture\n";
        }
        const auto hash = creator::core::sha256File(library);
        ASSERT_TRUE(hash.hasValue()) << hash.error().message();
        std::array<std::pair<std::string, std::string>, 2> runtimeDependencies{{
            {"bin/druntime-ldc-shared.dll", {}},
            {"bin/phobos2-ldc-shared.dll", {}},
        }};
        for (auto& [relative, dependencyHash] : runtimeDependencies) {
            const auto dependency = root_ / fs::path{relative};
            fs::create_directories(dependency.parent_path());
            std::ofstream output(dependency, std::ios::binary | std::ios::trunc);
            output << "real dependency fixture: " << relative;
            output.close();
            auto hashResult = creator::core::sha256File(dependency);
            ASSERT_TRUE(hashResult.hasValue()) << hashResult.error().message();
            dependencyHash = std::move(hashResult).value();
        }
        const auto thirdPartyHash =
            creator::core::sha256File(root_ / "THIRD_PARTY_NOTICES.txt");
        ASSERT_TRUE(thirdPartyHash.hasValue())
            << thirdPartyHash.error().message();

        std::ofstream output(root_ / "runtime-manifest.json",
                             std::ios::binary | std::ios::trunc);
        output
            << "{\"schema_version\":1,\"component\":\"Inochi2D C-FFI\","
               "\"version\":\"0.8.7-nightly+66fa768\","
               "\"source_commit\":\"66fa76834b28037db0c871c656563422f697879e\","
               "\"source_archive_sha256\":"
               "\"79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb\","
               "\"license\":\"BSD-2-Clause\",\"linking\":\"dynamic\","
               "\"dependencies\":{"
               "\"imagefmt\":{\"version\":\"2.1.2\",\"archive_sha256\":"
               "\"10f4182efc4fc3846561ca702b3207493f736639498b2dc61a3adcee2bb18736\"},"
               "\"inmath\":{\"version\":\"1.3.0\",\"archive_sha256\":"
               "\"865fa85d6c07c5f23207cdf9987207d95547e8303009cd0a028b8e7aa9d5aeae\"},"
               "\"intel-intrinsics\":{\"version\":\"1.12.1\",\"archive_sha256\":"
               "\"4e056612b6ebe819fef2e45c19d78427b7e67b3c8650445e7379ed2b30f61519\"},"
               "\"nulib\":{\"version\":\"0.3.5\",\"archive_sha256\":"
               "\"e4b56c28cd3264c72ba18e21889b9dddd1927b83828d92ebe6b49d559b22e597\"},"
               "\"numem\":{\"version\":\"1.3.2\",\"archive_sha256\":"
               "\"771688ea0ac4990e8576de4cdcdb381449d78d9edf7a6a7d55adeccfe46d94cc\"},"
               "\"silly\":{\"version\":\"1.1.1\",\"archive_sha256\":"
               "\"ffb78e740db5ab36c216c349ec36548a91c66fd1b69b980c1fd3e912ce8ae73b\"}},"
               "\"runtime_dependencies\":[{\"path\":\""
            << runtimeDependencies[0].first << "\",\"sha256\":\""
            << runtimeDependencies[0].second
            << "\",\"component\":\"LDC 1.40.0 BSL-1.0 runtime\"},"
               "{\"path\":\""
            << runtimeDependencies[1].first << "\",\"sha256\":\""
            << runtimeDependencies[1].second
            << "\",\"component\":\"LDC 1.40.0 BSL-1.0 runtime\"}],"
               "\"target\":\""
            << (targetOverride.empty() ? platform.target : targetOverride)
            << "\",\"target_triple\":\"" << platform.triple
            << "\",\"minimum_platform\":\"" << platform.minimumPlatform
            << "\",\"compiler\":\"LDC test fixture\","
               "\"sdk\":\"test fixture\",\"abi_mode\":\""
            << abiMode << "\",\"library\":{\"path\":\"" << platform.libraryPath
            << "\",\"sha256\":\"" << hash.value()
            << "\"},\"notice\":{\"path\":\"LICENSE\",\"sha256\":"
               "\"f79f6e26fa823e5c1881490bfee86627de43fc461ddeab4d80dc7af87cfc1743\""
               "},\"third_party_notices\":{\"path\":\"THIRD_PARTY_NOTICES.txt\","
               "\"sha256\":\""
            << thirdPartyHash.value() << "\"},\"symbols\":[";
        bool first = true;
        for (const auto symbol : kRequiredSymbols) {
            if (symbol == omittedSymbol) continue;
            if (!first) output << ',';
            output << '"' << symbol << '"';
            first = false;
        }
        output << "]}";
    }

    void overwriteLibraryByte() const {
        const auto library = root_ / fs::path{currentPlatform().libraryPath};
        std::fstream stream(library, std::ios::binary | std::ios::in |
                                         std::ios::out);
        ASSERT_TRUE(stream.is_open());
        stream.put('X');
    }

    void addExtraDynamicLibrary() const {
        const auto extra = root_ / "bin/extra.dll";
        fs::create_directories(extra.parent_path());
        std::ofstream output(extra, std::ios::binary);
        output << "unexpected";
    }

    void tamperThirdPartyNotices() const {
        std::ofstream output(root_ / "THIRD_PARTY_NOTICES.txt",
                             std::ios::binary | std::ios::trunc);
        output << "tampered";
    }

    void tamperRuntimeDependency() const {
        std::ofstream output(root_ / "bin/druntime-ldc-shared.dll",
                             std::ios::binary | std::ios::trunc);
        output << "tampered";
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

private:
    static void writeLibraryHeader(const fs::path& path,
                                   bool matchingArchitecture) {
        static_cast<void>(matchingArchitecture);
        std::vector<unsigned char> bytes;
#if defined(_WIN32) && defined(_M_X64)
        bytes.resize(128U);
        bytes[0] = 'M';
        bytes[1] = 'Z';
        bytes[0x3c] = 0x40;
        bytes[0x40] = 'P';
        bytes[0x41] = 'E';
        bytes[0x44] = matchingArchitecture ? 0x64 : 0x4c;
        bytes[0x45] = matchingArchitecture ? 0x86 : 0x01;
#elif defined(__APPLE__) && defined(__aarch64__)
        bytes = {0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0x00, 0x00, 0x01};
        if (!matchingArchitecture) bytes[4] = 0x07;
#elif defined(__ANDROID__) && defined(__aarch64__)
        bytes.resize(64U);
        bytes[0] = 0x7f;
        bytes[1] = 'E';
        bytes[2] = 'L';
        bytes[3] = 'F';
        bytes[4] = 2;
        bytes[5] = 1;
        bytes[18] = matchingArchitecture ? 0xb7 : 0x3e;
        bytes[19] = 0x00;
#endif
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    fs::path root_;
};

bool platformIsSupported() { return !currentPlatform().target.empty(); }

TEST(Inochi2dRuntimeManifestTest, AcceptsPinnedRuntimeWithoutLoadingLibrary) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime();

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().version, "0.8.7-nightly+66fa768");
    EXPECT_EQ(result.value().target, currentPlatform().target);
    EXPECT_EQ(result.value().libraryPath,
              fs::weakly_canonical(
                  fixture.root() / fs::path{currentPlatform().libraryPath}));
}

TEST(Inochi2dRuntimeManifestTest, RejectsChangedLibrary) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime();
    fixture.overwriteLibraryByte();

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
}

TEST(Inochi2dRuntimeManifestTest, RejectsMissingRequiredSymbol) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime("IN_VEC2_POSITION", {}, "in_puppet_draw");

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(Inochi2dRuntimeManifestTest, RejectsWrongAbiMode) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime("IN_VEC2_UV");

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(Inochi2dRuntimeManifestTest, RejectsUnexpectedDynamicLibrary) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime();
    fixture.addExtraDynamicLibrary();

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
}

TEST(Inochi2dRuntimeManifestTest, RejectsTargetMismatch) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime(
        "IN_VEC2_POSITION",
        currentPlatform().target == "windows-x64" ? "macos-arm64"
                                                  : "windows-x64");

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(Inochi2dRuntimeManifestTest, RejectsLibraryArchitectureMismatch) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime("IN_VEC2_POSITION", {}, {}, false);

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
}

TEST(Inochi2dRuntimeManifestTest, RejectsChangedThirdPartyNotices) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime();
    fixture.tamperThirdPartyNotices();

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
}

TEST(Inochi2dRuntimeManifestTest, RejectsChangedRuntimeDependency) {
    if (!platformIsSupported()) GTEST_SKIP() << "Unsupported bootstrap host";
    RuntimeFixture fixture;
    fixture.writeValidRuntime();
    fixture.tamperRuntimeDependency();

    const auto result = Inochi2dRuntimeManifest::loadAndVerify(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
}

}  // namespace
