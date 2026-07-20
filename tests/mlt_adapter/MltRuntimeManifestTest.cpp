#include "mlt_adapter/MltRuntimeManifest.h"
#include "mlt_adapter/MltEditEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* kHelloSha256 =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

#ifdef _WIN32
bool createDirectoryJunction(const fs::path& link, const fs::path& target) {
    std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.wstring() +
                           L"\" \"" + target.wstring() + L"\" >nul";
    std::vector<wchar_t> mutableCommand{command.begin(), command.end()};
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    const bool readExitCode = GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return readExitCode && exitCode == 0;
}
#endif

struct Entry final {
    std::string path;
    std::string hash{kHelloSha256};
    std::string role;
    std::string component{"MLT Framework"};
    std::string version{"7.40.0"};
    std::string sourceIdentity{
        "bef9d89c0c279e558d9625dac3399c2aa3d961bc"};
    std::string license{"LGPL-2.1-or-later"};
};

class RuntimeFixture final {
public:
    RuntimeFixture() {
        root_ = fs::temp_directory_path() /
                ("creator-studio-mlt-manifest-" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
        fs::create_directories(root_);
        entries_ = {{"bin/mlt-7.dll", kHelloSha256, "runtime-library"},
                    {"bin/mlt++-7.dll", kHelloSha256, "runtime-library"},
                    {"bin/z.dll", kHelloSha256, "runtime-library",
                     "zlib", "1.3.2",
                     "vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e",
                     "Zlib"},
                    {"lib/mlt-7/mltcore.dll", kHelloSha256, "runtime-module"},
                    {"lib/mlt-7/mltavformat.dll", kHelloSha256,
                     "runtime-module"}};
        for (const auto& entry : entries_) writeFile(entry.path, "hello");
        writeManifest();
    }
    ~RuntimeFixture() { fs::remove_all(root_); }

    void writeFile(const std::string& relative, const std::string& content) {
        const auto path = root_ / fs::path{relative};
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    void addEntry(Entry entry, const std::string& content = "hello") {
        writeFile(entry.path, content);
        entries_.push_back(std::move(entry));
        writeManifest();
    }

    void writeManifest(
        std::string version = "7.40.0",
        std::string windowsMutexPatch =
            "creator-studio-pthreads4w-v3.0.0-lazy-mutex-events-v1") {
        std::ofstream output(root_ / "mlt-runtime-manifest.json",
                             std::ios::binary | std::ios::trunc);
        output << "{\n\"abi\":1,\"component\":\"MLT Framework\","
                  "\"version\":\""
               << version
               << "\",\"source_commit\":"
                  "\"bef9d89c0c279e558d9625dac3399c2aa3d961bc\","
                  "\"windows_mutex_patch\":\""
               << windowsMutexPatch
               << "\","
                  "\"linking\":\"dynamic\","
                  "\"allowed_modules\":[\"core\",\"avformat\"],"
                  "\"dependencies\":["
                  "{\"component\":\"FFmpeg\",\"version\":\"8.1.2\","
                  "\"source_identity\":\"sha256:464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c\",\"license\":\"LGPL-2.1-or-later\"},"
                  "{\"component\":\"zlib\",\"version\":\"1.3.2\",\"source_identity\":\"vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e\",\"license\":\"Zlib\"},"
                  "{\"component\":\"PThreads4W\",\"version\":\"3.0.0\",\"source_identity\":\"vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e;patch:creator-studio-pthreads4w-v3.0.0-lazy-mutex-events-v1\",\"license\":\"Apache-2.0\"},"
                  "{\"component\":\"GNU libiconv\",\"version\":\"1.19\",\"source_identity\":\"vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e\",\"license\":\"LGPL-2.1-or-later\"},"
                  "{\"component\":\"dlfcn-win32\",\"version\":\"1.4.2\",\"source_identity\":\"vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e\",\"license\":\"MIT\"}],\"files\":[";
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (index != 0) output << ',';
            const auto& entry = entries_[index];
            output << "{\"path\":\"" << entry.path << "\",\"sha256\":\""
                   << entry.hash << "\",\"role\":\"" << entry.role
                   << "\",\"component\":\"" << entry.component
                   << "\",\"version\":\"" << entry.version
                   << "\",\"source_identity\":\"" << entry.sourceIdentity
                   << "\",\"license\":\"" << entry.license << "\"}";
        }
        output << "]}";
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    [[nodiscard]] std::vector<Entry>& entries() noexcept { return entries_; }

private:
    fs::path root_;
    std::vector<Entry> entries_;
};

TEST(MltRuntimeManifestTest, AcceptsExactAuditedFileSetAndHashes) {
    RuntimeFixture fixture;
    const auto result =
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
}

#ifdef _WIN32
TEST(MltRuntimeManifestTest, RejectsUnloadableLibrariesWithoutCrashingProcess) {
    RuntimeFixture fixture;

    const auto result =
        creator::mlt_adapter::MltEditEngine::preflightRuntime(fixture.root());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::UnsupportedVersion);
    EXPECT_NE(result.error().message().find("could not be loaded"),
              std::string::npos);
}
#endif

TEST(MltRuntimeManifestTest, RejectsChangedHash) {
    RuntimeFixture fixture;
    fixture.writeFile("bin/mlt-7.dll", "tampered");
    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root()).hasValue());
}

TEST(MltRuntimeManifestTest, RejectsFalsePerFileProvenance) {
    RuntimeFixture fixture;
    fixture.entries().front().license = "GPL-3.0-only";
    fixture.writeManifest();

    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root()).hasValue());
}

TEST(MltRuntimeManifestTest, RejectsMissingAndUnexpectedFiles) {
    RuntimeFixture missing;
    fs::remove(missing.root() / "bin/mlt-7.dll");
    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(missing.root()).hasValue());

    RuntimeFixture unexpected;
    unexpected.writeFile("bin/unexpected.dll", "hello");
    EXPECT_FALSE(creator::mlt_adapter::verifyMltRuntimeManifest(unexpected.root())
                     .hasValue());
}

TEST(MltRuntimeManifestTest, RejectsManifestWithoutRequiredZlibRuntime) {
    RuntimeFixture fixture;
    auto& entries = fixture.entries();
    std::erase_if(entries, [](const Entry& entry) {
        return entry.path == "bin/z.dll";
    });
    ASSERT_TRUE(fs::remove(fixture.root() / "bin/z.dll"));
    fixture.writeManifest();

    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root()).hasValue());
}

TEST(MltRuntimeManifestTest, RejectsWrongVersionDuplicateAndTraversal) {
    RuntimeFixture version;
    version.writeManifest("7.39.0");
    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(version.root()).hasValue());

    RuntimeFixture duplicate;
    duplicate.entries().push_back(duplicate.entries().front());
    duplicate.writeManifest();
    EXPECT_FALSE(creator::mlt_adapter::verifyMltRuntimeManifest(duplicate.root())
                     .hasValue());

    RuntimeFixture traversal;
    traversal.entries().front().path = "../mlt-7.dll";
    traversal.writeManifest();
    EXPECT_FALSE(creator::mlt_adapter::verifyMltRuntimeManifest(traversal.root())
                     .hasValue());
}

TEST(MltRuntimeManifestTest, RejectsUnapprovedWindowsMutexPatch) {
    RuntimeFixture fixture;
    fixture.writeManifest("7.40.0", "unapproved-local-patch");

    const auto result =
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root());
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::UnsupportedVersion);
}

TEST(MltRuntimeManifestTest, RejectsExecutableAndForbiddenModuleNames) {
    RuntimeFixture executable;
    executable.addEntry({"bin/helper.exe", kHelloSha256, "runtime-library"});
    EXPECT_FALSE(creator::mlt_adapter::verifyMltRuntimeManifest(executable.root())
                     .hasValue());

    RuntimeFixture gpl;
    gpl.addEntry({"lib/mlt-7/mltplusgpl.dll", kHelloSha256,
                  "runtime-module"});
    EXPECT_FALSE(
        creator::mlt_adapter::verifyMltRuntimeManifest(gpl.root()).hasValue());
}

#ifdef _WIN32
TEST(MltRuntimeManifestTest, RejectsReparsePointArtifact) {
    RuntimeFixture fixture;
    const auto link = fixture.root() / "redirected";
    const auto target = fixture.root() / "bin";
    ASSERT_TRUE(createDirectoryJunction(link, target));
    const auto result =
        creator::mlt_adapter::verifyMltRuntimeManifest(fixture.root());
    EXPECT_FALSE(result.hasValue());
    ASSERT_TRUE(fs::remove(link));
}
#endif

}  // namespace
