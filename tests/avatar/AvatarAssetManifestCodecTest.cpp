#include "avatar/AvatarAssetManifestCodec.h"

#include <gtest/gtest.h>

#include <nlohmann/json-schema.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace {

using namespace creator::avatar;
using creator::core::ErrorCode;
using creator::core::Utc;
using nlohmann::json;

Utc utc(std::string_view value) {
    return Utc::parseRfc3339(value).value();
}

std::vector<LicenseGrant> grants() {
    return {
        {AvatarRight::CommercialBroadcast, GrantState::Allowed, {}},
        {AvatarRight::AppBundle, GrantState::Allowed, {}},
        {AvatarRight::DerivativeCharacter, GrantState::Denied, {}},
        {AvatarRight::ModelExport, GrantState::Conditional, "vendor approval"},
        {AvatarRight::RawAssetRedistribution, GrantState::Denied, {}},
        {AvatarRight::PortableProject, GrantState::Allowed, {}},
        {AvatarRight::Attribution, GrantState::Allowed, {}},
    };
}

AvatarAssetManifestDraft validDraft() {
    return {
        .packageId = AvatarPackageId::create("vendor.foundation").value(),
        .packageVersion = "2.1.0",
        .assetId = AvatarAssetId::create("vendor.body.base").value(),
        .assetVersion = "3.2.1",
        .displayName = "기본 바디",
        .vendor = "Creator Studio",
        .supportedRepresentations = {AvatarRepresentation::Vrm1,
                                     AvatarRepresentation::Inochi2d},
        .supportedRigFamilies = {RigFamily::Kemonomimi, RigFamily::Humanoid},
        .allowedSlots = {AvatarSlot::Head, AvatarSlot::Body},
        .dependencies = {
            {AvatarAssetId::create("vendor.rig.base").value(), "1.2.0"},
            {AvatarAssetId::create("vendor.material.base").value(), "1.0.0"},
        },
        .payloads = {
            {"textures/body.png",
             "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
            {"models/body.vrm",
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        },
        .performance = {.payloadBytes = 8388608,
                        .textureBytes = 4194304,
                        .textureCount = 4,
                        .maxTextureDimension = 2048,
                        .vertexCount = 12000,
                        .triangleCount = 8000,
                        .drawCallCount = 6,
                        .drawPartCount = 0,
                        .boneCount = 72},
        .sourceUri = "https://creator-studio.local/assets/body",
        .licenseId = "vendor-commercial",
        .licenseVersion = "4.0.0",
        .grants = grants(),
        .attributionText = "Body by Creator Studio",
        .regionAllowList = {"US", "KR"},
        .validFrom = utc("2026-01-01T00:00:00Z"),
        .validUntil = utc("2027-01-01T00:00:00Z"),
    };
}

AvatarAssetManifest validManifest() {
    return AvatarAssetManifest::create(validDraft()).value();
}

json committedSchema() {
    std::ifstream stream{CS_AVATAR_ASSET_SCHEMA_PATH, std::ios::binary};
    return json::parse(stream);
}

nlohmann::json_schema::json_validator committedValidator() {
    auto schema = committedSchema();
    auto& performance =
        schema["definitions"]["performance"]["properties"];
    for (const auto* field : {"payloadBytes", "textureBytes"}) {
        performance[field].erase("minimum");
        performance[field].erase("maximum");
    }
    nlohmann::json_schema::json_validator validator{
        nullptr, nlohmann::json_schema::default_string_format_check};
    validator.set_root_schema(schema);
    return validator;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

std::size_t temporarySiblingCount(const std::filesystem::path& path) {
    const auto parent =
        path.parent_path().empty() ? std::filesystem::current_path()
                                   : path.parent_path();
    const auto prefix = "." + path.filename().string() + ".part-";
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator{parent}) {
        if (entry.path().filename().string().starts_with(prefix)) {
            ++count;
        }
    }
    return count;
}

std::string hashFor(std::size_t index) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(64) << index;
    return stream.str();
}

TEST(AvatarAssetManifestTest, CanonicalRoundTripSortsEverySetLikeCollection) {
    const auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    const auto decoded = AvatarAssetManifestCodec{}.fromJson(document);
    ASSERT_TRUE(decoded.hasValue()) << decoded.error().message();
    EXPECT_EQ(AvatarAssetManifestCodec{}.toJson(decoded.value()), document);
    EXPECT_EQ(document["supportedRepresentations"][0], "inochi2d");
    EXPECT_EQ(document["supportedRigFamilies"][0], "humanoid");
    EXPECT_EQ(document["allowedSlots"][0], "body");
    EXPECT_EQ(document["dependencies"][0]["assetId"], "vendor.material.base");
    EXPECT_EQ(document["payloads"][0]["path"], "models/body.vrm");
    EXPECT_EQ(document["grants"][0]["right"], "commercial-broadcast");
    EXPECT_EQ(document["regionAllowList"][0], "KR");
    EXPECT_EQ(document["performance"]["payloadBytes"], 8388608);
    EXPECT_EQ(document["performance"]["textureCount"], 4);
    EXPECT_EQ(document["performance"]["maxTextureDimension"], 2048);
    EXPECT_EQ(document["performance"]["drawCallCount"], 6);
    EXPECT_EQ(document["performance"]["drawPartCount"], 0);
    EXPECT_EQ(document["performance"]["boneCount"], 72);
}

TEST(AvatarAssetManifestTest, CanonicalDocumentValidatesAgainstCommittedDraft7Schema) {
    auto validator = committedValidator();
    EXPECT_NO_THROW(validator.validate(AvatarAssetManifestCodec{}.toJson(validManifest())));
}

TEST(AvatarAssetManifestTest, RejectsUnknownFieldsAndNativeWidthFutureVersions) {
    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    document["surprise"] = true;
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::ParseFailure);
    document.erase("surprise");

    document["schemaVersion"] = static_cast<std::int64_t>(4294967297LL);
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::UnsupportedVersion);
    document["schemaVersion"] = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
              ErrorCode::UnsupportedVersion);
}

TEST(AvatarAssetManifestTest, RejectsNonpositiveAndNonintegerVersions) {
    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    for (const json version : {json{0}, json{-1}, json{1.0}}) {
        document["schemaVersion"] = version;
        EXPECT_EQ(AvatarAssetManifestCodec{}.fromJson(document).error().code(),
                  ErrorCode::ParseFailure);
    }
}

TEST(AvatarAssetManifestTest, SchemaAndDecoderPreserveExactUint64Boundaries) {
    const auto schema = committedSchema();
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto& properties =
        schema["definitions"]["performance"]["properties"];
    ASSERT_TRUE(properties["payloadBytes"].contains("maximum"));
    ASSERT_TRUE(properties["textureBytes"].contains("maximum"));
    EXPECT_EQ(properties["payloadBytes"]["maximum"].get<std::uint64_t>(),
              maximum);
    EXPECT_EQ(properties["textureBytes"]["maximum"].get<std::uint64_t>(),
              maximum);

    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    document["performance"]["payloadBytes"] = maximum;
    document["performance"]["textureBytes"] = maximum;
    const auto boundary =
        AvatarAssetManifestCodec{}.fromJson(json::parse(document.dump()));
    ASSERT_TRUE(boundary.hasValue()) << boundary.error().message();
    EXPECT_EQ(boundary.value().values().performance.payloadBytes, maximum);
    EXPECT_EQ(boundary.value().values().performance.textureBytes, maximum);

    const auto overflow = json::parse("18446744073709551616");
    for (const auto* field : {"payloadBytes", "textureBytes"}) {
        document = AvatarAssetManifestCodec{}.toJson(validManifest());
        document["performance"][field] = overflow;
        const auto decoded = AvatarAssetManifestCodec{}.fromJson(document);
        ASSERT_FALSE(decoded.hasValue());
        EXPECT_EQ(decoded.error().code(), ErrorCode::ParseFailure);
    }
}

TEST(AvatarAssetManifestTest, FactoryRejectsDuplicateDeclarations) {
    auto draft = validDraft();
    draft.payloads.push_back(draft.payloads.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.payloads[1].sha256 = draft.payloads[0].sha256;
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.allowedSlots.push_back(draft.allowedSlots.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.supportedRigFamilies.push_back(draft.supportedRigFamilies.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.supportedRepresentations.push_back(draft.supportedRepresentations.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.grants.push_back(draft.grants.front());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsUnsafePayloadPaths) {
    for (const std::string path :
         {"", "/absolute.bin", "//rooted.bin", "C:/drive.bin",
          "C:\\drive.bin", "payload\\file.bin", ".", "..",
          "payload/./file.bin", "payload/../file.bin", "payload//file.bin",
          "payload/"}) {
        auto draft = validDraft();
        draft.payloads[0].path = path;
        EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue())
            << path;
    }

    auto draft = validDraft();
    draft.payloads.push_back(
        {"models/./body.vrm",
         "1111111111111111111111111111111111111111111111111111111111111111"});
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, PayloadPathsTreatPercentEscapesAsLiteralText) {
    auto draft = validDraft();
    draft.payloads[0].path = "payload/%2e%2e/file.bin";
    EXPECT_TRUE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
    document["payloads"][0]["path"] = "payload/%2e%2e/file.bin";
    auto validator = committedValidator();
    EXPECT_NO_THROW(validator.validate(document));
}

TEST(AvatarAssetManifestTest, SchemaRejectsUnsafePayloadPaths) {
    auto validator = committedValidator();
    for (const std::string path :
         {"", "/absolute.bin", "//rooted.bin", "C:/drive.bin",
          "C:\\drive.bin", "payload\\file.bin", ".", "..",
          "payload/./file.bin", "payload/../file.bin", "payload//file.bin",
          "payload/"}) {
        auto document = AvatarAssetManifestCodec{}.toJson(validManifest());
        document["payloads"][0]["path"] = path;
        EXPECT_THROW(validator.validate(document), std::exception) << path;
    }
}

TEST(AvatarAssetManifestTest, FactoryRejectsInvalidNamesVersionsAndHashes) {
    auto draft = validDraft();
    draft.displayName = std::string{"\xC0\x80", 2};
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.assetVersion = "1.0";
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.payloads[0].sha256[0] = 'A';
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, AttributionTextIsAlwaysBoundedValidUtf8) {
    auto draft = validDraft();
    for (auto& grant : draft.grants) {
        if (grant.right == AvatarRight::Attribution) {
            grant.state = GrantState::Denied;
        }
    }
    draft.attributionText = std::string{"\xC0\x80", 2};
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    for (auto& grant : draft.grants) {
        if (grant.right == AvatarRight::Attribution) {
            grant.state = GrantState::Denied;
        }
    }
    draft.attributionText.assign(1001, 'a');
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.attributionText.assign(1000, 'a');
    EXPECT_TRUE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.attributionText.clear();
    for (std::size_t index = 0; index < 1000; ++index) {
        draft.attributionText += "\xEA\xB0\x80";
    }
    EXPECT_TRUE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsEveryOutOfDomainEnum) {
    auto draft = validDraft();
    draft.supportedRepresentations[0] =
        static_cast<AvatarRepresentation>(999);
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.supportedRigFamilies[0] = static_cast<RigFamily>(999);
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.allowedSlots[0] = static_cast<AvatarSlot>(999);
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.grants[0].right = static_cast<AvatarRight>(999);
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.grants[0].state = static_cast<GrantState>(999);
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsMissingCompatibilityOrPayload) {
    auto draft = validDraft();
    draft.supportedRepresentations.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.supportedRigFamilies.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.allowedSlots.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.payloads.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, FactoryRejectsEmptyAttributionAndInvalidValidityWindow) {
    auto draft = validDraft();
    draft.attributionText.clear();
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
    draft = validDraft();
    draft.validUntil = draft.validFrom;
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, CoreAssetsRequireExplicitNonconditionalCommercialGrants) {
    auto draft = validDraft();
    draft.assetId = AvatarAssetId::create("core.body.base").value();
    draft.grants.erase(draft.grants.begin());
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());

    draft = validDraft();
    draft.assetId = AvatarAssetId::create("core.body.base").value();
    draft.grants[0] = {AvatarRight::CommercialBroadcast, GrantState::Conditional,
                       "approval"};
    EXPECT_FALSE(AvatarAssetManifest::create(std::move(draft)).hasValue());
}

TEST(AvatarAssetManifestTest, SaveAndLoadUseCanonicalJson) {
    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-manifest.json";
    ASSERT_TRUE(AvatarAssetManifestCodec{}.save(path, validManifest()).hasValue());
    const auto loaded = AvatarAssetManifestCodec{}.load(path);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    std::ifstream stream{path, std::ios::binary};
    const std::string persisted{std::istreambuf_iterator<char>{stream},
                                std::istreambuf_iterator<char>{}};
    EXPECT_EQ(persisted,
              AvatarAssetManifestCodec{}.toJson(loaded.value()).dump(2));
    stream.close();
    std::filesystem::remove(path);
}

TEST(AvatarAssetManifestTest, LoadRejectsDecodedDuplicateObjectMembers) {
    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-duplicate.json";
    const auto canonical = AvatarAssetManifestCodec{}.toJson(validManifest()).dump();

    auto duplicate = canonical;
    duplicate.insert(1, R"("vendor":"shadow",)");
    writeText(path, duplicate);
    auto loaded = AvatarAssetManifestCodec{}.load(path);
    EXPECT_FALSE(loaded.hasValue());
    if (!loaded.hasValue()) {
        EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
    }

    duplicate = canonical;
    duplicate.insert(1, R"("gr\u0061nts":[],)");
    writeText(path, duplicate);
    loaded = AvatarAssetManifestCodec{}.load(path);
    EXPECT_FALSE(loaded.hasValue());
    if (!loaded.hasValue()) {
        EXPECT_EQ(loaded.error().code(), ErrorCode::ParseFailure);
    }
    std::filesystem::remove(path);
}

TEST(AvatarAssetManifestTest, OversizedSaveFailsBeforeCreatingTemporaryFile) {
    auto draft = validDraft();
    draft.payloads.clear();
    for (std::size_t index = 0; index < 9000; ++index) {
        std::ostringstream path;
        path << "payload/" << std::string(970, 'a') << std::setw(5)
             << std::setfill('0') << index << ".bin";
        draft.payloads.push_back({path.str(), hashFor(index)});
    }
    const auto manifest = AvatarAssetManifest::create(std::move(draft));
    ASSERT_TRUE(manifest.hasValue()) << manifest.error().message();

    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-too-large-save.json";
    std::filesystem::remove(path);
    const auto before = temporarySiblingCount(path);
    const auto saved = AvatarAssetManifestCodec{}.save(path, manifest.value());
    EXPECT_FALSE(saved.hasValue());
    if (!saved.hasValue()) {
        EXPECT_EQ(saved.error().code(), ErrorCode::ParseFailure);
    }
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(temporarySiblingCount(path), before);
    std::filesystem::remove(path);
}

TEST(AvatarAssetManifestTest, RejectsTraversalMalformedAndOversizedInput) {
    EXPECT_EQ(AvatarAssetManifestCodec{}
                  .load(std::filesystem::path{".."} / "manifest.json")
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(AvatarAssetManifestCodec{}
                  .save(std::filesystem::path{".."} / "manifest.json",
                        validManifest())
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);

    const auto path = std::filesystem::path{testing::TempDir()} /
                      "avatar-asset-invalid.json";
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << "{ malformed";
    }
    EXPECT_EQ(AvatarAssetManifestCodec{}.load(path).error().code(),
              ErrorCode::ParseFailure);
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream << std::string(8U * 1024U * 1024U + 1U, ' ');
    }
    EXPECT_EQ(AvatarAssetManifestCodec{}.load(path).error().code(),
              ErrorCode::ParseFailure);
    std::filesystem::remove(path);
}

}  // namespace
