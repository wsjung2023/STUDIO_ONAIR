#include "avatar_pack_adapter/FileAvatarCatalog.h"
#include "avatar_pack_adapter/FileAvatarCatalogInternal.h"
#include "SignedAvatarPackFixture.h"
#include "core/Uuid.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <array>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <Aclapi.h>
#include <Windows.h>
#include <process.h>
#include <winioctl.h>
#endif

namespace creator::avatar_pack_adapter {
namespace {

namespace fs = std::filesystem;
using test::SignedAvatarPackFixture;
using test::SignedPackOptions;

TEST(FileAvatarCatalogInternalTest, UsesAStrictTwentyFourHourBoundary) {
    const auto now = fs::file_time_type{
        fs::file_time_type::duration{42'000'000'000LL}};
    const auto threshold = std::chrono::hours{24};

    EXPECT_FALSE(detail::isAbandonedStagingEntry(now - threshold, now));
    EXPECT_FALSE(detail::isAbandonedStagingEntry(
        now - threshold + fs::file_time_type::duration{1}, now));
    EXPECT_TRUE(detail::isAbandonedStagingEntry(
        now - threshold - fs::file_time_type::duration{1}, now));
}

TEST(FileAvatarCatalogInternalTest,
     IndeterminatePromotionNeverBecomesARetryableInstallFailure) {
    const auto durable = detail::reconciledInstallOutcome(
        PromotionOutcome::Durable, true);
    ASSERT_TRUE(durable.hasValue());
    EXPECT_EQ(durable.value(), CatalogInstallOutcome::Installed);

    const auto reconciled = detail::reconciledInstallOutcome(
        PromotionOutcome::Indeterminate, true);
    ASSERT_TRUE(reconciled.hasValue());
    EXPECT_EQ(reconciled.value(),
              CatalogInstallOutcome::InstalledDurabilityIndeterminate);

    const auto uncertain = detail::reconciledInstallOutcome(
        PromotionOutcome::Indeterminate, false);
    ASSERT_FALSE(uncertain.hasValue());
    EXPECT_EQ(uncertain.error().code(), core::ErrorCode::IoFailure);
    ASSERT_TRUE(uncertain.error().issueCode().has_value());
    EXPECT_EQ(*uncertain.error().issueCode(),
              "avatar.catalog.reconciliation-required");
}

TEST(FileAvatarCatalogInternalTest,
     AcceptsOnlyPortablePackageIdsAndCanonicalVersions) {
    EXPECT_TRUE(detail::isPortablePackageId("vendor.foundation"));
    EXPECT_TRUE(detail::isPortablePackageId("vendor.avatar_pack-2"));
    EXPECT_FALSE(detail::isPortablePackageId(""));
    EXPECT_FALSE(detail::isPortablePackageId("."));
    EXPECT_FALSE(detail::isPortablePackageId(".."));
    EXPECT_FALSE(detail::isPortablePackageId("/absolute"));
    EXPECT_FALSE(detail::isPortablePackageId("C:/absolute"));
    EXPECT_FALSE(detail::isPortablePackageId("../escape"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor/pack"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor\\pack"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor:pack"));
    EXPECT_FALSE(detail::isPortablePackageId("Vendor.foundation"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor..foundation"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor."));
    EXPECT_FALSE(detail::isPortablePackageId("vendor "));
    EXPECT_FALSE(detail::isPortablePackageId("vendor.foundati\u00f3n"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor.con"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor.com1"));
    EXPECT_FALSE(detail::isPortablePackageId("vendor.com\u00b9"));
    EXPECT_FALSE(detail::isPortablePackageId(
        std::string(129U, 'a')));

    EXPECT_TRUE(detail::isCanonicalPackageVersion("0.0.0"));
    EXPECT_TRUE(detail::isCanonicalPackageVersion("1.2.3-alpha.1+build-7"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("01.2.3"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.02.3"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.03"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("v1.2.3"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.3-ALPHA"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.3+BUILD"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.3/escape"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.3\\escape"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion("1.2.3:escape"));
    EXPECT_FALSE(detail::isCanonicalPackageVersion(
        std::string(129U, '1')));
}

class FileAvatarCatalogTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("creator-avatar-catalog-" + core::generateUuidV4());
        ASSERT_TRUE(fs::create_directories(root_));
#ifndef _WIN32
        fs::permissions(root_, fs::perms::owner_all,
                        fs::perm_options::replace);
#endif
        fixture_ =
            std::make_unique<SignedAvatarPackFixture>(root_ / "packs");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    FileAvatarCatalog openCatalog() {
        return openCatalogAt(root_ / "catalog");
    }

    FileAvatarCatalog openCatalogAt(const fs::path& catalogRoot) {
        auto opened = FileAvatarCatalog::open(
            catalogRoot, {fixture_->trustedKey()});
        if (!opened.hasValue())
            throw std::runtime_error{opened.error().message()};
        return std::move(opened).value();
    }

    static avatar::AvatarAssetId assetId(
        std::string_view value = "core.body.humanoid") {
        auto id = avatar::AvatarAssetId::create(std::string{value});
        if (!id.hasValue()) throw std::runtime_error{id.error().message()};
        return std::move(id).value();
    }

    fs::path root_;
    std::unique_ptr<SignedAvatarPackFixture> fixture_;
};

fs::path installedVersion(const fs::path& root,
                          std::string_view packageId = "vendor.foundation",
                          std::string_view packageVersion = "1.0.0") {
    return root / "catalog" / "installed" / std::string{packageId} /
           std::string{packageVersion};
}

#ifdef _WIN32
std::error_code createDirectoryJunction(const fs::path& junction,
                                        const fs::path& target) {
    struct MountPointReparseData final {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };

    if (!CreateDirectoryW(junction.c_str(), nullptr)) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    const std::wstring printName = fs::absolute(target).wstring();
    const std::wstring substituteName = L"\\??\\" + printName;
    const auto substituteBytes =
        static_cast<WORD>(substituteName.size() * sizeof(WCHAR));
    const auto printBytes =
        static_cast<WORD>(printName.size() * sizeof(WCHAR));
    constexpr std::size_t kMountPointHeaderBytes = 8U;
    const std::size_t pathBytes =
        static_cast<std::size_t>(substituteBytes) + sizeof(WCHAR) +
        printBytes + sizeof(WCHAR);
    alignas(void*) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE>
        storage{};
    auto* data =
        reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength =
        static_cast<WORD>(kMountPointHeaderBytes + pathBytes);
    data->substituteNameOffset = 0;
    data->substituteNameLength = substituteBytes;
    data->printNameOffset =
        static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    data->printNameLength = printBytes;
    std::memcpy(data->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte*>(data->pathBuffer) +
                    data->printNameOffset,
                printName.data(), printBytes);

    const HANDLE handle = CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
        RemoveDirectoryW(junction.c_str());
        return error;
    }
    DWORD returned = 0;
    const DWORD inputBytes = static_cast<DWORD>(8U + data->dataLength);
    const BOOL created =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, inputBytes,
                        nullptr, 0, &returned, nullptr);
    const DWORD code = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!created) RemoveDirectoryW(junction.c_str());
    return {static_cast<int>(code), std::system_category()};
}

DWORD grantEveryoneWriteAccess(const fs::path& path,
                               DWORD inheritance =
                                   SUB_CONTAINERS_AND_OBJECTS_INHERIT) {
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> everyone{};
    DWORD sidBytes = static_cast<DWORD>(everyone.size());
    if (CreateWellKnownSid(WinWorldSid, nullptr, everyone.data(),
                           &sidBytes) == FALSE) {
        return GetLastError();
    }
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto status = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr,
        &descriptor);
    if (status != ERROR_SUCCESS) return status;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_WRITE | DELETE | WRITE_DAC;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = inheritance;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName =
        reinterpret_cast<LPWSTR>(everyone.data());
    PACL updated = nullptr;
    status = SetEntriesInAclW(1U, &access, oldDacl, &updated);
    if (status == ERROR_SUCCESS) {
        status = SetNamedSecurityInfoW(
            const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    }
    if (updated != nullptr) (void)LocalFree(updated);
    if (descriptor != nullptr) (void)LocalFree(descriptor);
    return status;
}
#endif

TEST_F(FileAvatarCatalogTest, OpenNeverCreatesMissingIntermediateAncestors) {
    const auto missing = root_ / "missing" / "ancestor";

    auto opened = FileAvatarCatalog::open(
        missing / "catalog", {fixture_->trustedKey()});

    EXPECT_FALSE(opened.hasValue());
    EXPECT_FALSE(fs::exists(root_ / "missing"));
}

TEST_F(FileAvatarCatalogTest,
       RetainedLockAuthorityRejectsAnInodeSplit) {
    auto catalog = openCatalog();
    const auto lockPath = root_ / "catalog" / "catalog.lock";
    ASSERT_TRUE(fs::remove(lockPath));
    {
        std::ofstream replacement{lockPath, std::ios::binary};
        replacement << "replacement";
    }
#ifndef _WIN32
    fs::permissions(lockPath, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
#endif

    const auto listed = catalog.list();

    EXPECT_FALSE(listed.hasValue());
}

#ifndef _WIN32
TEST_F(FileAvatarCatalogTest,
       RetainedRootAuthorityRejectsRenameAndReplacement) {
    auto catalog = openCatalog();
    const auto catalogRoot = root_ / "catalog";
    const auto displaced = root_ / "catalog-displaced";
    fs::rename(catalogRoot, displaced);
    ASSERT_TRUE(fs::create_directory(catalogRoot));
    fs::permissions(catalogRoot, fs::perms::owner_all,
                    fs::perm_options::replace);

    const auto listed = catalog.list();

    EXPECT_FALSE(listed.hasValue());
}
#endif

TEST_F(FileAvatarCatalogTest, InstallsARealSignedPackAndPublishesItAtomically) {
    auto catalog = openCatalog();
    const auto package = fixture_->writePack();

    const auto installed = catalog.install(package);

    ASSERT_TRUE(installed.hasValue()) << installed.error().message();
    EXPECT_EQ(installed.value(), CatalogInstallOutcome::Installed);
    const auto manifests = catalog.list();
    ASSERT_TRUE(manifests.hasValue()) << manifests.error().message();
    ASSERT_EQ(manifests.value().size(), 1U);
    EXPECT_EQ(manifests.value()[0].assetId().value(),
              "core.body.humanoid");
}

TEST_F(FileAvatarCatalogTest,
       RejectsSignedPackageIdentityBeforeUsingItAsAPath) {
    auto catalog = openCatalog();
    const std::array<std::string_view, 14U> unsafeIds{
        "/absolute",
        "C:/absolute",
        "..",
        "../escape",
        "vendor/pack",
        "vendor\\pack",
        "vendor:pack",
        "Vendor.foundation",
        "vendor.foundati\u00f3n",
        "vendor.foundatio\u0301n",
        "vendor.con",
        "vendor.com\u00b9",
        "vendor.",
        "vendor ",
    };
    for (const auto unsafeId : unsafeIds) {
        SCOPED_TRACE(unsafeId);
        SignedPackOptions unsafe;
        unsafe.packageId = unsafeId;
        const auto rejected =
            catalog.install(fixture_->writePack(std::move(unsafe)));
        EXPECT_FALSE(rejected.hasValue());
        EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "installed"));
        EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "staging"));
    }
    EXPECT_FALSE(fs::exists(root_ / "catalog" / "escape"));
    const auto listed = catalog.list();
    ASSERT_TRUE(listed.hasValue()) << listed.error().message();
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(FileAvatarCatalogTest,
       RejectsAnActuallySignedNonCanonicalPackageVersion) {
    auto catalog = openCatalog();
    SignedPackOptions unsafe;
    unsafe.rawManifestPackageVersion = "1.0.0-ALPHA";

    const auto rejected =
        catalog.install(fixture_->writePack(std::move(unsafe)));

    EXPECT_FALSE(rejected.hasValue());
    EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "installed"));
    EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "staging"));
}

TEST_F(FileAvatarCatalogTest,
       InstallPreflightsEveryPhysicalPackageAlias) {
    auto catalog = openCatalog();
    const auto installed = root_ / "catalog" / "installed";
    const std::array<fs::path, 4U> aliases{
        L"Vendor.foundation",
        L"vendor.foundati\u00f3n",
        L"vendor.foundatio\u0301n",
        L"vendor.con",
    };

    for (std::size_t index = 0U; index < aliases.size(); ++index) {
        SCOPED_TRACE(index);
        const auto aliasPath = installed / aliases[index];
        ASSERT_TRUE(fs::create_directory(aliasPath));
        const auto rejected = catalog.install(fixture_->writePack());
        EXPECT_FALSE(rejected.hasValue());
        EXPECT_FALSE(fs::exists(installedVersion(root_)));
        EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "staging"));
        ASSERT_TRUE(fs::remove(aliasPath));
    }
}

TEST_F(FileAvatarCatalogTest,
       InstallPreflightsEveryPhysicalVersionAlias) {
    auto catalog = openCatalog();
    const auto package =
        root_ / "catalog" / "installed" / "vendor.foundation";
    ASSERT_TRUE(fs::create_directory(package));
    ASSERT_TRUE(fs::create_directory(package / "1.0.0-ALPHA"));

    const auto rejected = catalog.install(fixture_->writePack());

    EXPECT_FALSE(rejected.hasValue());
    EXPECT_FALSE(fs::exists(installedVersion(root_)));
    EXPECT_TRUE(fs::is_empty(root_ / "catalog" / "staging"));
}

TEST_F(FileAvatarCatalogTest, FailedSignedUpgradeLeavesPreviousVersionUsable) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    SignedPackOptions tampered;
    tampered.packageVersion = "1.1.0";
    tampered.assetVersion = "1.1.0";
    tampered.corruptSignature = true;

    const auto rejected =
        catalog.install(fixture_->writePack(std::move(tampered)));

    ASSERT_FALSE(rejected.hasValue());
    const auto previous = catalog.find(assetId(), "1.0.0");
    ASSERT_TRUE(previous.hasValue()) << previous.error().message();
    EXPECT_EQ(previous.value().values().packageVersion, "1.0.0");
}

TEST_F(FileAvatarCatalogTest,
       DeclaredPayloadByteMismatchNeverDisturbsInstalledVersion) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    SignedPackOptions mismatch;
    mismatch.packageVersion = "1.1.0";
    mismatch.assetVersion = "1.1.0";
    mismatch.declaredPayloadBytes = 1U;

    const auto rejected =
        catalog.install(fixture_->writePack(std::move(mismatch)));

    ASSERT_FALSE(rejected.hasValue());
    EXPECT_TRUE(catalog.find(assetId(), "1.0.0").hasValue());
    EXPECT_FALSE(fs::exists(
        installedVersion(root_, "vendor.foundation", "1.1.0")));
}

TEST_F(FileAvatarCatalogTest,
       RehashesInstalledPayloadBeforeReturningAuthority) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    const auto installedPayload =
        installedVersion(root_) / "payload" / "model.bin";
    std::ofstream output{installedPayload,
                         std::ios::binary | std::ios::trunc};
    output << "tampered";
    output.close();
    ASSERT_TRUE(output);

    const auto payload = catalog.payloadRoot(assetId(), "1.0.0");

    ASSERT_FALSE(payload.hasValue());
    EXPECT_EQ(payload.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest,
       SameCanonicalManifestIsIdempotentAndDifferentManifestConflicts) {
    auto catalog = openCatalog();
    const auto first = fixture_->writePack();
    ASSERT_TRUE(catalog.install(first).hasValue());

    const auto idempotent = catalog.install(first);
    ASSERT_TRUE(idempotent.hasValue()) << idempotent.error().message();
    EXPECT_EQ(idempotent.value(), CatalogInstallOutcome::AlreadyInstalled);

    SignedPackOptions conflict;
    conflict.payloads.push_back(
        {.path = "payload/model.bin",
         .contents = {'d', 'i', 'f', 'f', 'e', 'r', 'e', 'n', 't'}});
    const auto rejected =
        catalog.install(fixture_->writePack(std::move(conflict)));
    ASSERT_FALSE(rejected.hasValue());
    EXPECT_EQ(rejected.error().code(), core::ErrorCode::AlreadyExists);

    const auto payload = catalog.payloadRoot(assetId(), "1.0.0");
    ASSERT_TRUE(payload.hasValue()) << payload.error().message();
    std::ifstream input{payload.value() / "model.bin", std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input},
                               std::istreambuf_iterator<char>{}};
    EXPECT_EQ(contents, "real-avatar-model");
}

TEST_F(FileAvatarCatalogTest,
       RejectsDuplicateAssetVersionMappingBeforePublishingSecondPackage) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    SignedPackOptions duplicate;
    duplicate.packageId = "vendor.duplicate";

    const auto rejected =
        catalog.install(fixture_->writePack(std::move(duplicate)));

    ASSERT_FALSE(rejected.hasValue());
    EXPECT_EQ(rejected.error().code(), core::ErrorCode::IoFailure);
    EXPECT_FALSE(fs::exists(root_ / "catalog" / "installed" /
                            "vendor.duplicate"));
    EXPECT_TRUE(catalog.find(assetId(), "1.0.0").hasValue());
}

TEST_F(FileAvatarCatalogTest, ListsAssetsInCanonicalDeterministicOrder) {
    auto catalog = openCatalog();
    SignedPackOptions later;
    later.packageId = "vendor.zeta";
    later.assetId = "core.zeta";
    SignedPackOptions earlier;
    earlier.packageId = "vendor.alpha";
    earlier.assetId = "core.alpha";
    ASSERT_TRUE(
        catalog.install(fixture_->writePack(std::move(later))).hasValue());
    ASSERT_TRUE(
        catalog.install(fixture_->writePack(std::move(earlier))).hasValue());

    const auto manifests = catalog.list();

    ASSERT_TRUE(manifests.hasValue()) << manifests.error().message();
    ASSERT_EQ(manifests.value().size(), 2U);
    EXPECT_EQ(manifests.value()[0].assetId().value(), "core.alpha");
    EXPECT_EQ(manifests.value()[1].assetId().value(), "core.zeta");
}

TEST_F(FileAvatarCatalogTest, ListAndFindFailClosedOnTamperedManifest) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    std::ofstream output{installedVersion(root_) / "manifest.json",
                         std::ios::binary | std::ios::trunc};
    output << R"({"schemaVersion":1,"tampered":true})";
    output.close();
    ASSERT_TRUE(output);

    const auto listed = catalog.list();
    const auto found = catalog.find(assetId(), "1.0.0");

    ASSERT_FALSE(listed.hasValue());
    EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
    ASSERT_FALSE(found.hasValue());
    EXPECT_EQ(found.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest,
       ListFailsClosedWhenAValidShapeManifestNoLongerMatchesItsSignature) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    const auto manifestPath = installedVersion(root_) / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream input{manifestPath, std::ios::binary};
        input >> manifest;
    }
    manifest["performance"]["payloadBytes"] =
        manifest["performance"]["payloadBytes"].get<std::uint64_t>() + 1U;
    {
        std::ofstream output{manifestPath,
                             std::ios::binary | std::ios::trunc};
        output << manifest.dump();
    }

    const auto listed = catalog.list();
    const auto payload = catalog.payloadRoot(assetId(), "1.0.0");

    ASSERT_FALSE(listed.hasValue());
    EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
    ASSERT_FALSE(payload.hasValue());
    EXPECT_EQ(payload.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest, IdempotentInstallRejectsACorruptTarget) {
    auto catalog = openCatalog();
    const auto package = fixture_->writePack();
    ASSERT_TRUE(catalog.install(package).hasValue());
    std::ofstream output{installedVersion(root_) / "payload" / "model.bin",
                         std::ios::binary | std::ios::trunc};
    output << "tampered";
    output.close();

    const auto reinstalled = catalog.install(package);

    ASSERT_FALSE(reinstalled.hasValue());
    EXPECT_EQ(reinstalled.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest,
       PayloadAuthorityRejectsMissingAndAddedTopology) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    const auto payload = installedVersion(root_) / "payload";
    ASSERT_TRUE(fs::remove(payload / "model.bin"));
    auto missing = catalog.payloadRoot(assetId(), "1.0.0");
    ASSERT_FALSE(missing.hasValue());
    EXPECT_EQ(missing.error().code(), core::ErrorCode::IoFailure);

    std::ofstream restored{payload / "model.bin", std::ios::binary};
    restored << "real-avatar-model";
    restored.close();
    std::ofstream extra{payload / "undeclared.bin", std::ios::binary};
    extra << "extra";
    extra.close();
    ASSERT_TRUE(restored);
    ASSERT_TRUE(extra);

    auto added = catalog.payloadRoot(assetId(), "1.0.0");
    ASSERT_FALSE(added.hasValue());
    EXPECT_EQ(added.error().code(), core::ErrorCode::IoFailure);
}

#ifdef _WIN32
TEST_F(FileAvatarCatalogTest,
       RejectsAnOtherUserWritableCatalogParent) {
    const auto parent = root_ / "unsafe-parent";
    const auto catalogRoot = parent / "catalog";
    ASSERT_TRUE(fs::create_directories(catalogRoot));
    ASSERT_EQ(grantEveryoneWriteAccess(parent, NO_INHERITANCE),
              ERROR_SUCCESS);

    auto opened = FileAvatarCatalog::open(
        catalogRoot, {fixture_->trustedKey()});

    EXPECT_FALSE(opened.hasValue());
}

TEST_F(FileAvatarCatalogTest,
       RetainedRootAuthorityPreventsRenameAndReplacement) {
    auto catalog = openCatalog();
    const auto catalogRoot = root_ / "catalog";
    const auto displaced = root_ / "catalog-displaced";

    const auto renamed =
        MoveFileExW(catalogRoot.c_str(), displaced.c_str(), 0U);

    EXPECT_EQ(renamed, FALSE);
    if (renamed != FALSE) {
        ASSERT_NE(MoveFileExW(displaced.c_str(), catalogRoot.c_str(), 0U),
                  FALSE);
    }
    const auto listed = catalog.list();
    ASSERT_TRUE(listed.hasValue()) << listed.error().message();
}

TEST_F(FileAvatarCatalogTest,
       PayloadAuthorityRejectsAReparseDirectoryWithoutTraversal) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    const auto outside = root_ / "outside-payload";
    ASSERT_TRUE(fs::create_directories(outside));
    {
        std::ofstream marker{outside / "marker.bin", std::ios::binary};
        marker << "outside";
    }
    const auto linked = installedVersion(root_) / "payload" / "linked";
    const auto junctionError = createDirectoryJunction(linked, outside);
    ASSERT_FALSE(junctionError) << junctionError.message();

    const auto payload = catalog.payloadRoot(assetId(), "1.0.0");

    ASSERT_FALSE(payload.hasValue());
    EXPECT_EQ(payload.error().code(), core::ErrorCode::IoFailure);
    EXPECT_TRUE(fs::exists(outside / "marker.bin"));
}

TEST_F(FileAvatarCatalogTest, ListRejectsAReparseInstalledVersion) {
    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    const auto version = installedVersion(root_);
    const auto replacement = root_ / "version-replacement";
    ASSERT_FALSE(
        MoveFileExW(version.c_str(), replacement.c_str(), 0U) == FALSE);
    const auto junctionError = createDirectoryJunction(version, replacement);
    ASSERT_FALSE(junctionError) << junctionError.message();

    const auto listed = catalog.list();

    ASSERT_FALSE(listed.hasValue());
    EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest, RejectsCatalogAndPackageDirectoriesWithUnsafeAcl) {
    const auto unsafeRoot = root_ / "unsafe-catalog";
    ASSERT_TRUE(fs::create_directories(unsafeRoot));
    ASSERT_EQ(grantEveryoneWriteAccess(unsafeRoot), ERROR_SUCCESS);

    auto rejectedRoot = FileAvatarCatalog::open(
        unsafeRoot, {fixture_->trustedKey()});

    EXPECT_FALSE(rejectedRoot.hasValue());

    auto catalog = openCatalog();
    ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    ASSERT_EQ(grantEveryoneWriteAccess(installedVersion(root_)),
              ERROR_SUCCESS);

    const auto listed = catalog.list();
    ASSERT_FALSE(listed.hasValue());
    EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
}

TEST_F(FileAvatarCatalogTest,
       RejectsUnsafeLockPayloadDirectoryAndReplacedInstalledRoot) {
    {
        auto catalog = openCatalog();
        ASSERT_EQ(grantEveryoneWriteAccess(
                      root_ / "catalog" / "catalog.lock"),
                  ERROR_SUCCESS);
        const auto listed = catalog.list();
        ASSERT_FALSE(listed.hasValue());
        EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
    }

    const auto secondRoot = root_ / "second-catalog";
    auto opened = FileAvatarCatalog::open(
        secondRoot, {fixture_->trustedKey()});
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto secondCatalog = std::move(opened).value();
    SignedPackOptions nested;
    nested.payloads.push_back(
        {.path = "payload/models/model.bin",
         .contents = {'m', 'o', 'd', 'e', 'l'}});
    ASSERT_TRUE(secondCatalog
                    .install(fixture_->writePack(std::move(nested)))
                    .hasValue());
    ASSERT_EQ(grantEveryoneWriteAccess(
                  secondRoot / "installed" / "vendor.foundation" / "1.0.0" /
                  "payload" / "models"),
              ERROR_SUCCESS);
    const auto unsafePayload =
        secondCatalog.payloadRoot(assetId(), "1.0.0");
    ASSERT_FALSE(unsafePayload.hasValue());
    EXPECT_EQ(unsafePayload.error().code(), core::ErrorCode::IoFailure);

    const auto installed = secondRoot / "installed";
    const auto replacement = root_ / "installed-replacement";
    ASSERT_NE(MoveFileExW(installed.c_str(), replacement.c_str(), 0U), FALSE);
    const auto junctionError =
        createDirectoryJunction(installed, replacement);
    ASSERT_FALSE(junctionError) << junctionError.message();
    const auto replaced = secondCatalog.list();
    ASSERT_FALSE(replaced.hasValue());
    EXPECT_EQ(replaced.error().code(), core::ErrorCode::IoFailure);
}
#endif

TEST_F(FileAvatarCatalogTest, SupportsAUnicodeCatalogRoot) {
    const auto unicodeRoot = root_ / fs::path{L"\uCE74\uD0C8\uB85C\uADF8-\uC544\uBC14\uD0C0"};
    auto catalog = openCatalogAt(unicodeRoot);

    const auto installed = catalog.install(fixture_->writePack());

    ASSERT_TRUE(installed.hasValue()) << installed.error().message();
    const auto payload = catalog.payloadRoot(assetId(), "1.0.0");
    ASSERT_TRUE(payload.hasValue()) << payload.error().message();
    EXPECT_TRUE(fs::exists(payload.value() / "model.bin"));
}

TEST_F(FileAvatarCatalogTest,
       ConcurrentListObservesOnlyAbsentOrFullyPublishedInstall) {
    auto installer = openCatalog();
    auto reader = openCatalog();
    SignedPackOptions options;
    options.payloads.push_back(
        {.path = "payload/model.bin",
         .contents = std::vector<std::uint8_t>(8U * 1024U * 1024U, 0x5aU)});
    const auto package = fixture_->writePack(std::move(options));
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> installed{false};
    std::thread installThread{[&] {
        started.store(true, std::memory_order_release);
        const auto result = installer.install(package);
        installed.store(result.hasValue(), std::memory_order_release);
        finished.store(true, std::memory_order_release);
    }};
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();

    bool invalidObservation = false;
    do {
        const auto listed = reader.list();
        if (!listed.hasValue() || listed.value().size() > 1U)
            invalidObservation = true;
        if (listed.hasValue() && listed.value().size() == 1U) {
            const auto& manifest = listed.value().front();
            if (manifest.values().performance.payloadBytes !=
                    8U * 1024U * 1024U ||
                manifest.values().payloads.size() != 1U) {
                invalidObservation = true;
            }
        }
    } while (!finished.load(std::memory_order_acquire));
    installThread.join();

    EXPECT_TRUE(installed.load(std::memory_order_acquire));
    EXPECT_FALSE(invalidObservation);
    const auto listed = reader.list();
    ASSERT_TRUE(listed.hasValue()) << listed.error().message();
    ASSERT_EQ(listed.value().size(), 1U);
}

TEST_F(FileAvatarCatalogTest,
       StartupRemovesOnlyStaleStagingAndNeverTouchesQuarantine) {
    {
        auto catalog = openCatalog();
        (void)catalog;
    }
    const auto catalogRoot = root_ / "catalog";
    const auto oldStaging = catalogRoot / "staging" / "old-staging";
    const auto freshStaging = catalogRoot / "staging" / "fresh-staging";
    const auto quarantined = catalogRoot / "quarantine" / "old-quarantine";
    ASSERT_TRUE(fs::create_directories(oldStaging / "payload"));
    ASSERT_TRUE(fs::create_directories(freshStaging / "payload"));
    ASSERT_TRUE(fs::create_directories(quarantined / "payload"));
    const auto now = fs::file_time_type::clock::now();
    std::error_code error;
    fs::last_write_time(oldStaging, now - std::chrono::hours{25}, error);
    ASSERT_FALSE(error) << error.message();
    fs::last_write_time(freshStaging, now - std::chrono::hours{23}, error);
    ASSERT_FALSE(error) << error.message();
    fs::last_write_time(quarantined, now - std::chrono::hours{48}, error);
    ASSERT_FALSE(error) << error.message();

    auto reopened = FileAvatarCatalog::open(
        catalogRoot, {fixture_->trustedKey()});

    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    EXPECT_FALSE(fs::exists(oldStaging));
    EXPECT_TRUE(fs::exists(freshStaging));
    EXPECT_TRUE(fs::exists(quarantined));
}

TEST_F(FileAvatarCatalogTest,
       StartupReconciliationRejectsACorruptInstalledTarget) {
    {
        auto catalog = openCatalog();
        ASSERT_TRUE(catalog.install(fixture_->writePack()).hasValue());
    }
    std::ofstream output{installedVersion(root_) / "payload" / "model.bin",
                         std::ios::binary | std::ios::trunc};
    output << "tampered";
    output.close();

    auto reopened = FileAvatarCatalog::open(
        root_ / "catalog", {fixture_->trustedKey()});

    ASSERT_FALSE(reopened.hasValue());
    EXPECT_EQ(reopened.error().code(), core::ErrorCode::IoFailure);
    ASSERT_TRUE(reopened.error().issueCode().has_value());
}

#ifdef _WIN32
TEST_F(FileAvatarCatalogTest, StartupCleanupFailureIsSurfaced) {
    {
        auto catalog = openCatalog();
        (void)catalog;
    }
    const auto catalogRoot = root_ / "catalog";
    const auto abandoned = catalogRoot / "staging" / "blocked";
    ASSERT_TRUE(fs::create_directories(abandoned));
    const auto blockedFile = abandoned / "model.bin";
    {
        std::ofstream output{blockedFile, std::ios::binary};
        output << "blocked";
    }
    std::error_code error;
    fs::last_write_time(abandoned,
                        fs::file_time_type::clock::now() -
                            std::chrono::hours{25},
                        error);
    ASSERT_FALSE(error) << error.message();
    const HANDLE blocker = CreateFileW(
        blockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(blocker, INVALID_HANDLE_VALUE);

    auto reopened = FileAvatarCatalog::open(
        catalogRoot, {fixture_->trustedKey()});

    EXPECT_FALSE(reopened.hasValue());
    EXPECT_TRUE(fs::exists(abandoned));
    EXPECT_NE(CloseHandle(blocker), FALSE);
}

TEST_F(FileAvatarCatalogTest, CrossProcessExclusiveLockRejectsContention) {
    auto catalog = openCatalog();
    const auto lockPath = root_ / "catalog" / "catalog.lock";
    const auto readyPath = root_ / "lock-ready";
    const auto releasePath = root_ / "lock-release";
    const auto process = _spawnl(
        _P_NOWAIT, CS_AVATAR_CATALOG_LOCK_FIXTURE_PATH,
        CS_AVATAR_CATALOG_LOCK_FIXTURE_PATH, lockPath.string().c_str(),
        readyPath.string().c_str(), releasePath.string().c_str(), nullptr);
    ASSERT_NE(process, -1);
    bool ready = false;
    for (int attempt = 0; attempt < 1'000; ++attempt) {
        if (fs::exists(readyPath)) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_TRUE(ready);

    const auto listed = catalog.list();

    EXPECT_FALSE(listed.hasValue());
    if (!listed.hasValue()) {
        EXPECT_EQ(listed.error().code(), core::ErrorCode::IoFailure);
        ASSERT_TRUE(listed.error().issueCode().has_value());
        EXPECT_EQ(*listed.error().issueCode(), "avatar.catalog.lock.busy");
    }
    {
        std::ofstream release{releasePath, std::ios::binary};
        release << "release";
    }
    int status = -1;
    ASSERT_NE(_cwait(&status, process, 0), -1);
    EXPECT_EQ(status, 0);
}

TEST_F(FileAvatarCatalogTest,
       StartupRejectsStaleReparseWithoutTouchingItsTarget) {
    {
        auto catalog = openCatalog();
        (void)catalog;
    }
    const auto catalogRoot = root_ / "catalog";
    const auto outside = root_ / "outside-staging";
    ASSERT_TRUE(fs::create_directories(outside));
    {
        std::ofstream marker{outside / "marker.bin", std::ios::binary};
        marker << "outside";
    }
    std::error_code error;
    fs::last_write_time(outside,
                        fs::file_time_type::clock::now() -
                            std::chrono::hours{25},
                        error);
    ASSERT_FALSE(error) << error.message();
    const auto junction = catalogRoot / "staging" / "replacement";
    const auto junctionError = createDirectoryJunction(junction, outside);
    ASSERT_FALSE(junctionError) << junctionError.message();

    auto reopened = FileAvatarCatalog::open(
        catalogRoot, {fixture_->trustedKey()});

    ASSERT_FALSE(reopened.hasValue());
    EXPECT_TRUE(fs::exists(junction));
    EXPECT_TRUE(fs::exists(outside / "marker.bin"));
}
#endif

}  // namespace
}  // namespace creator::avatar_pack_adapter
