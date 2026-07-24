#include "avatar/AvatarAssetManifestCodec.h"
#include "avatar_pack_adapter/AvatarPackValidator.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <gtest/gtest.h>
#include <miniz.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::avatar_pack_adapter {
namespace {

namespace fs = std::filesystem;
using avatar::AvatarAssetManifest;
using avatar::AvatarAssetManifestCodec;
using avatar::AvatarAssetManifestDraft;
using avatar::AvatarPayloadHash;
using avatar::AvatarRepresentation;
using avatar::AvatarRight;
using avatar::AvatarSlot;
using avatar::GrantState;
using avatar::LicenseGrant;
using avatar::RigFamily;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct ZipEntry final {
    std::string path;
    std::vector<std::uint8_t> contents;
    mz_uint compression{MZ_NO_COMPRESSION};
};

struct Payload final {
    std::string path;
    std::vector<std::uint8_t> contents;
};

std::vector<std::uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

std::string digest(std::span<const std::uint8_t> value) {
    core::Sha256 hash;
    hash.update(value);
    return hash.finish();
}

std::array<std::uint8_t, crypto_hash_sha256_BYTES> decodeDigest(
    std::string_view value) {
    std::array<std::uint8_t, crypto_hash_sha256_BYTES> decoded{};
    std::size_t decodedLength = 0;
    if (sodium_hex2bin(decoded.data(), decoded.size(), value.data(), value.size(),
                       nullptr, &decodedLength, nullptr) != 0 ||
        decodedLength != decoded.size()) {
        throw std::runtime_error{"test digest could not be decoded"};
    }
    return decoded;
}

void writeBytes(const fs::path& path, std::span<const std::uint8_t> contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error{"test file could not be created"};
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) throw std::runtime_error{"test file could not be written"};
}

std::vector<std::uint8_t> readBytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"test file could not be opened"};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::uint16_t read16(const std::vector<std::uint8_t>& value,
                     std::size_t offset) {
    return static_cast<std::uint16_t>(value.at(offset)) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(value.at(offset + 1U)) << 8U);
}

std::uint32_t read32(const std::vector<std::uint8_t>& value,
                     std::size_t offset) {
    return static_cast<std::uint32_t>(value.at(offset)) |
           (static_cast<std::uint32_t>(value.at(offset + 1U)) << 8U) |
           (static_cast<std::uint32_t>(value.at(offset + 2U)) << 16U) |
           (static_cast<std::uint32_t>(value.at(offset + 3U)) << 24U);
}

void write16(std::vector<std::uint8_t>& value, std::size_t offset,
             std::uint16_t replacement) {
    value.at(offset) = static_cast<std::uint8_t>(replacement & 0xffU);
    value.at(offset + 1U) =
        static_cast<std::uint8_t>((replacement >> 8U) & 0xffU);
}

void write32(std::vector<std::uint8_t>& value, std::size_t offset,
             std::uint32_t replacement) {
    value.at(offset) = static_cast<std::uint8_t>(replacement & 0xffU);
    value.at(offset + 1U) =
        static_cast<std::uint8_t>((replacement >> 8U) & 0xffU);
    value.at(offset + 2U) =
        static_cast<std::uint8_t>((replacement >> 16U) & 0xffU);
    value.at(offset + 3U) =
        static_cast<std::uint8_t>((replacement >> 24U) & 0xffU);
}

struct CentralEntry final {
    std::size_t offset{};
    std::size_t nameOffset{};
    std::size_t nameLength{};
    std::size_t localOffset{};
};

std::vector<CentralEntry> centralEntries(
    const std::vector<std::uint8_t>& archive) {
    constexpr std::uint32_t kCentralSignature = 0x02014b50U;
    std::vector<CentralEntry> result;
    for (std::size_t offset = 0; offset + 46U <= archive.size();) {
        if (read32(archive, offset) != kCentralSignature) {
            ++offset;
            continue;
        }
        const auto nameLength = read16(archive, offset + 28U);
        const auto extraLength = read16(archive, offset + 30U);
        const auto commentLength = read16(archive, offset + 32U);
        const std::size_t next = offset + 46U + nameLength + extraLength +
                                 commentLength;
        if (next > archive.size())
            throw std::runtime_error{"invalid test central directory"};
        result.push_back(
            {.offset = offset,
             .nameOffset = offset + 46U,
             .nameLength = nameLength,
             .localOffset = read32(archive, offset + 42U)});
        offset = next;
    }
    return result;
}

std::string centralName(const std::vector<std::uint8_t>& archive,
                        const CentralEntry& entry) {
    return {reinterpret_cast<const char*>(archive.data() + entry.nameOffset),
            entry.nameLength};
}

CentralEntry findCentral(const std::vector<std::uint8_t>& archive,
                         std::string_view path) {
    const auto entries = centralEntries(archive);
    const auto found =
        std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return centralName(archive, entry) == path;
        });
    if (found == entries.end())
        throw std::runtime_error{"test ZIP entry was not found"};
    return *found;
}

void patchSizes(const fs::path& path, std::string_view entryPath,
                std::optional<std::uint32_t> compressed,
                std::optional<std::uint32_t> uncompressed) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (compressed.has_value())
        write32(archive, entry.offset + 20U, *compressed);
    if (uncompressed.has_value())
        write32(archive, entry.offset + 24U, *uncompressed);
    writeBytes(path, archive);
}

void patchMethod(const fs::path& path, std::string_view entryPath,
                 std::uint16_t method) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 10U, method);
    write16(archive, entry.localOffset + 8U, method);
    writeBytes(path, archive);
}

void patchEncrypted(const fs::path& path, std::string_view entryPath) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 8U,
            static_cast<std::uint16_t>(
                read16(archive, entry.offset + 8U) | 1U));
    write16(archive, entry.localOffset + 6U,
            static_cast<std::uint16_t>(
                read16(archive, entry.localOffset + 6U) | 1U));
    writeBytes(path, archive);
}

void patchUnixMode(const fs::path& path, std::string_view entryPath,
                   std::uint16_t mode) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 4U, 0x0314U);
    write32(archive, entry.offset + 38U,
            static_cast<std::uint32_t>(mode) << 16U);
    writeBytes(path, archive);
}

void patchNameByte(const fs::path& path, std::string_view entryPath,
                   std::size_t index, std::uint8_t replacement) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (index >= entry.nameLength)
        throw std::runtime_error{"test filename patch is out of range"};
    const auto localNameLength = read16(archive, entry.localOffset + 26U);
    if (localNameLength != entry.nameLength)
        throw std::runtime_error{"test local filename length differs"};
    archive.at(entry.nameOffset + index) = replacement;
    archive.at(entry.localOffset + 30U + index) = replacement;
    writeBytes(path, archive);
}

void patchName(const fs::path& path, std::string_view entryPath,
               std::string_view replacement) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (replacement.size() != entry.nameLength)
        throw std::runtime_error{"test filename patch changes length"};
    const auto localNameLength = read16(archive, entry.localOffset + 26U);
    if (localNameLength != entry.nameLength)
        throw std::runtime_error{"test local filename length differs"};
    std::copy(replacement.begin(), replacement.end(),
              archive.begin() +
                  static_cast<std::ptrdiff_t>(entry.nameOffset));
    std::copy(replacement.begin(), replacement.end(),
              archive.begin() +
                  static_cast<std::ptrdiff_t>(entry.localOffset + 30U));
    writeBytes(path, archive);
}

void corruptStoredPayload(const fs::path& path, std::string_view entryPath) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (read16(archive, entry.offset + 10U) != 0U)
        throw std::runtime_error{"test payload is not stored"};
    const auto nameLength = read16(archive, entry.localOffset + 26U);
    const auto extraLength = read16(archive, entry.localOffset + 28U);
    const auto dataOffset =
        entry.localOffset + 30U + nameLength + extraLength;
    archive.at(dataOffset) ^= 0x5aU;
    writeBytes(path, archive);
}

bool directoryEmpty(const fs::path& path) {
    return fs::directory_iterator{path} == fs::directory_iterator{};
}

class AvatarPackValidatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GE(sodium_init(), 0);
        root_ = fs::temp_directory_path() /
                ("creator-avatar-pack-" + core::generateUuidV4());
        staging_ = root_ / "staging";
        ASSERT_TRUE(fs::create_directories(staging_));
        ASSERT_EQ(crypto_sign_keypair(publicKey_.data(), secretKey_.data()), 0);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    TrustedAvatarKey trustedKey(std::string keyId = "test.release") const {
        TrustedAvatarKey key{.keyId = std::move(keyId)};
        std::transform(publicKey_.begin(), publicKey_.end(),
                       key.publicKey.begin(), [](std::uint8_t value) {
                           return static_cast<std::byte>(value);
                       });
        return key;
    }

    AvatarPackValidator validator() const {
        return AvatarPackValidator{{trustedKey()}, staging_};
    }

    fs::path uniquePack() {
        return root_ / (core::generateUuidV4() + ".csavatarpack");
    }

    fs::path writeArchive(const std::vector<ZipEntry>& entries) {
        const auto path = uniquePack();
        mz_zip_archive archive{};
        mz_zip_zero_struct(&archive);
        if (!mz_zip_writer_init_file(&archive, path.string().c_str(), 0U))
            throw std::runtime_error{"miniz writer could not open fixture"};
        bool ended = false;
        try {
            for (const auto& entry : entries) {
                const void* data = entry.contents.empty()
                                       ? static_cast<const void*>("")
                                       : entry.contents.data();
                if (!mz_zip_writer_add_mem(&archive, entry.path.c_str(), data,
                                           entry.contents.size(),
                                           entry.compression)) {
                    throw std::runtime_error{"miniz could not add fixture entry"};
                }
            }
            if (!mz_zip_writer_finalize_archive(&archive))
                throw std::runtime_error{"miniz could not finalize fixture"};
            if (!mz_zip_writer_end(&archive))
                throw std::runtime_error{"miniz could not close fixture"};
            ended = true;
        } catch (...) {
            if (!ended) mz_zip_writer_end(&archive);
            throw;
        }
        return path;
    }

    AvatarAssetManifest manifestFor(
        const std::vector<Payload>& payloads,
        std::optional<std::uint64_t> declaredBytes = std::nullopt,
        std::optional<std::string> firstDigest = std::nullopt) {
        std::vector<AvatarPayloadHash> hashes;
        std::uint64_t payloadBytes = 0;
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            payloadBytes += payloads[index].contents.size();
            hashes.push_back(
                {.path = payloads[index].path,
                 .sha256 = index == 0U && firstDigest.has_value()
                               ? *firstDigest
                               : digest(payloads[index].contents)});
        }
        auto packageId =
            avatar::AvatarPackageId::create("vendor.foundation");
        auto assetId =
            avatar::AvatarAssetId::create("core.body.humanoid");
        if (!packageId.hasValue() || !assetId.hasValue())
            throw std::runtime_error{"test identifier is invalid"};
        const auto validFrom =
            core::Utc::parseRfc3339("2026-07-24T00:00:00Z");
        if (!validFrom.hasValue())
            throw std::runtime_error{"test timestamp is invalid"};
        AvatarAssetManifestDraft draft{
            .packageId = packageId.value(),
            .packageVersion = "1.0.0",
            .assetId = assetId.value(),
            .assetVersion = "1.0.0",
            .displayName = "Foundation Body",
            .vendor = "Creator Studio",
            .supportedRepresentations = {AvatarRepresentation::GltfRig},
            .supportedRigFamilies = {RigFamily::Humanoid},
            .allowedSlots = {AvatarSlot::Body},
            .payloads = std::move(hashes),
            .performance =
                {.payloadBytes = declaredBytes.value_or(payloadBytes),
                 .textureBytes = 0,
                 .textureCount = 0,
                 .maxTextureDimension = 0,
                 .vertexCount = 1,
                 .triangleCount = 1,
                 .drawCallCount = 1,
                 .drawPartCount = 1,
                 .boneCount = 1},
            .sourceUri = "https://example.invalid/foundation",
            .licenseId = "commercial-test",
            .licenseVersion = "1.0.0",
            .grants =
                {{.right = AvatarRight::CommercialBroadcast,
                  .state = GrantState::Allowed,
                  .condition = ""},
                 {.right = AvatarRight::AppBundle,
                  .state = GrantState::Allowed,
                  .condition = ""},
                 {.right = AvatarRight::DerivativeCharacter,
                  .state = GrantState::Allowed,
                  .condition = ""}},
            .attributionText = "",
            .regionAllowList = {},
            .validFrom = validFrom.value(),
            .validUntil = std::nullopt,
        };
        auto manifest = AvatarAssetManifest::create(std::move(draft));
        if (!manifest.hasValue())
            throw std::runtime_error{manifest.error().message()};
        return std::move(manifest).value();
    }

    std::vector<std::uint8_t> signatureMessage(
        const AvatarAssetManifest& manifest) {
        const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
        std::vector<std::uint8_t> message{canonical.begin(), canonical.end()};
        auto payloads = manifest.values().payloads;
        std::sort(payloads.begin(), payloads.end(),
                  [](const auto& left, const auto& right) {
                      return left.path < right.path;
                  });
        for (const auto& payload : payloads) {
            message.insert(message.end(), payload.path.begin(),
                           payload.path.end());
            message.push_back(0U);
            const auto decoded = decodeDigest(payload.sha256);
            message.insert(message.end(), decoded.begin(), decoded.end());
        }
        return message;
    }

    fs::path writeSignedPack(
        const std::vector<Payload>& payloads,
        std::optional<std::uint64_t> declaredBytes = std::nullopt,
        std::optional<std::string> firstDigest = std::nullopt,
        std::string keyId = "test.release",
        std::optional<std::size_t> signatureSize = std::nullopt,
        bool corruptSignature = false,
        std::vector<ZipEntry> extraEntries = {},
        std::optional<std::string> omitPayload = std::nullopt) {
        const auto manifest =
            manifestFor(payloads, declaredBytes, std::move(firstDigest));
        const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
        const auto message = signatureMessage(manifest);
        std::array<std::uint8_t, crypto_sign_BYTES> signature{};
        unsigned long long signatureLength = 0;
        if (crypto_sign_detached(signature.data(), &signatureLength,
                                 message.data(), message.size(),
                                 secretKey_.data()) != 0 ||
            signatureLength != signature.size()) {
            throw std::runtime_error{"libsodium could not sign fixture"};
        }
        if (corruptSignature) signature[0] ^= 0x80U;
        const auto outputSignatureSize =
            signatureSize.value_or(signature.size());
        std::vector<ZipEntry> entries{
            {.path = "manifest.json", .contents = bytes(canonical)},
            {.path = "signature.ed25519",
             .contents =
                 std::vector<std::uint8_t>(
                     signature.begin(),
                     signature.begin() +
                         std::min(outputSignatureSize, signature.size()))},
            {.path = "signing-key-id.txt", .contents = bytes(keyId)},
        };
        if (outputSignatureSize > signature.size()) {
            entries[1].contents.resize(outputSignatureSize, 0U);
        }
        for (const auto& payload : payloads) {
            if (omitPayload.has_value() && payload.path == *omitPayload)
                continue;
            entries.push_back({.path = payload.path,
                               .contents = payload.contents});
        }
        entries.insert(entries.end(),
                       std::make_move_iterator(extraEntries.begin()),
                       std::make_move_iterator(extraEntries.end()));
        return writeArchive(entries);
    }

    fs::path writeSignedPack(
        std::string path = "payload/model.bin",
        std::string_view contents = "real-avatar-model") {
        return writeSignedPack(
            {{.path = std::move(path), .contents = bytes(contents)}});
    }

    fs::path root_;
    fs::path staging_;
    std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey_{};
    std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> secretKey_{};
};

TEST_F(AvatarPackValidatorTest, ExtractsARealValidSignedFixture) {
    const auto package = writeSignedPack();

    const auto result = validator().validateAndExtract(package);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().manifest.assetId().value(),
              "core.body.humanoid");
    EXPECT_TRUE(fs::is_regular_file(result.value().stagingRoot /
                                    "manifest.json"));
    EXPECT_EQ(readBytes(result.value().stagingRoot / "signature.ed25519")
                  .size(),
              crypto_sign_BYTES);
    EXPECT_EQ(readBytes(result.value().stagingRoot / "signing-key-id.txt"),
              bytes("test.release"));
    EXPECT_EQ(readBytes(result.value().stagingRoot / "payload/model.bin"),
              bytes("real-avatar-model"));
}

TEST_F(AvatarPackValidatorTest,
       ExtractsCanonicalUtf8PathWithoutSystemCodePageConversion) {
    const std::string utf8Path = "payload/\xf0\x9f\x90\xb1.bin";
    const auto package = writeSignedPack(utf8Path, "model");

    const auto result = validator().validateAndExtract(package);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    const std::u8string nativePath{
        reinterpret_cast<const char8_t*>(utf8Path.data()), utf8Path.size()};
    EXPECT_EQ(readBytes(result.value().stagingRoot / fs::path{nativePath}),
              bytes("model"));
}

TEST_F(AvatarPackValidatorTest, RejectsUnsafeRawEntryPathsBeforeExtraction) {
    const std::vector<std::string> unsafe{
        "../escape",       "payload/../escape", "payload/./model",
        "payload//model",  "payload/model/",    "/absolute",
        "//server/share",  "C:/drive",           "payload\\model",
        std::string{"payload/"} + static_cast<char>(1) + "model",
    };
    const auto outside = root_ / "outside-sentinel";
    writeBytes(outside, bytes("untouched"));
    for (const auto& path : unsafe) {
        const std::string placeholder(path.size(), 'x');
        const auto package =
            writeArchive({{.path = placeholder, .contents = bytes("x")}});
        patchName(package, placeholder, path);
        const auto result = validator().validateAndExtract(package);
        EXPECT_FALSE(result.hasValue()) << path;
        if (!result.hasValue())
            EXPECT_EQ(result.error().code(),
                      core::ErrorCode::InvalidArgument)
                << path;
        EXPECT_TRUE(directoryEmpty(staging_)) << path;
    }
    EXPECT_EQ(readBytes(outside), bytes("untouched"));
}

TEST_F(AvatarPackValidatorTest, RejectsEmbeddedNulInRawZipName) {
    const std::string original = "payload/xmodel";
    const auto package =
        writeArchive({{.path = original, .contents = bytes("x")}});
    patchNameByte(package, original, std::string_view{"payload/"}.size(), 0U);

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDuplicateCaseAndUnicodeNormalizedEntryNames) {
    const auto duplicate = writeArchive(
        {{.path = "same", .contents = bytes("one")},
         {.path = "same", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(duplicate).hasValue());

    const auto caseCollision = writeSignedPack(
        {{.path = "payload/Model.bin", .contents = bytes("one")},
         {.path = "payload/model.bin", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(caseCollision).hasValue());

    const auto unicodeCollision = writeSignedPack(
        {{.path = "payload/caf\xc3\xa9.bin", .contents = bytes("one")},
         {.path = "payload/cafe\xcc\x81.bin", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(unicodeCollision).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsCaseInsensitiveFileDirectoryPrefixCollisionBeforeStaging) {
    const auto package = writeSignedPack(
        {{.path = "Payload", .contents = bytes("one")},
         {.path = "payload/model.bin", .contents = bytes("two")}});

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDirectorySymlinkEncryptionAndUnsupportedMethods) {
    const auto directory =
        writeArchive({{.path = "payload/", .contents = {}}});
    EXPECT_FALSE(validator().validateAndExtract(directory).hasValue());

    auto symlink =
        writeArchive({{.path = "payload/link", .contents = bytes("target")}});
    patchUnixMode(symlink, "payload/link", 0120777U);
    EXPECT_FALSE(validator().validateAndExtract(symlink).hasValue());

    auto encrypted =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchEncrypted(encrypted, "payload/model");
    EXPECT_FALSE(validator().validateAndExtract(encrypted).hasValue());

    auto unsupported =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchMethod(unsupported, "payload/model", 99U);
    EXPECT_FALSE(validator().validateAndExtract(unsupported).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsEntryCountAndIndividualOrAggregateZipBombMetadata) {
    std::vector<ZipEntry> tooMany;
    tooMany.reserve(10'001U);
    for (std::size_t index = 0; index < 10'001U; ++index) {
        tooMany.push_back(
            {.path = "payload/" + std::to_string(index), .contents = {}});
    }
    EXPECT_FALSE(
        validator().validateAndExtract(writeArchive(tooMany)).hasValue());

    auto individual =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchSizes(individual, "payload/model", std::nullopt,
               static_cast<std::uint32_t>(512ULL * kMiB + 1ULL));
    EXPECT_FALSE(validator().validateAndExtract(individual).hasValue());

    auto compressed =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchSizes(compressed, "payload/model",
               static_cast<std::uint32_t>(512ULL * kMiB + 1ULL),
               std::nullopt);
    EXPECT_FALSE(validator().validateAndExtract(compressed).hasValue());

    std::vector<ZipEntry> five;
    for (std::size_t index = 0; index < 5U; ++index) {
        five.push_back(
            {.path = "payload/" + std::to_string(index),
             .contents = bytes("x")});
    }
    auto aggregateExpanded = writeArchive(five);
    auto aggregateCompressed = writeArchive(five);
    for (std::size_t index = 0; index < 5U; ++index) {
        const auto name = "payload/" + std::to_string(index);
        patchSizes(aggregateExpanded, name, std::nullopt,
                   static_cast<std::uint32_t>(500ULL * kMiB));
        patchSizes(aggregateCompressed, name,
                   static_cast<std::uint32_t>(500ULL * kMiB),
                   std::nullopt);
    }
    EXPECT_FALSE(
        validator().validateAndExtract(aggregateExpanded).hasValue());
    EXPECT_FALSE(
        validator().validateAndExtract(aggregateCompressed).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, RejectsOversizedBoundedMetadata) {
    std::vector<std::uint8_t> oversizedManifest(
        static_cast<std::size_t>(8ULL * kMiB + 1ULL), 'x');
    const auto package = writeArchive(
        {{.path = "manifest.json",
          .contents = std::move(oversizedManifest),
          .compression = MZ_BEST_COMPRESSION},
         {.path = "signature.ed25519",
          .contents = std::vector<std::uint8_t>(crypto_sign_BYTES)},
         {.path = "signing-key-id.txt", .contents = bytes("test.release")}});

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsTruncationCrcMismatchAndSignatureTamperWithCleanup) {
    auto truncated = readBytes(writeSignedPack());
    truncated.resize(truncated.size() - 12U);
    const auto truncatedPath = uniquePack();
    writeBytes(truncatedPath, truncated);
    EXPECT_FALSE(validator().validateAndExtract(truncatedPath).hasValue());

    auto crcMismatch = writeSignedPack();
    corruptStoredPayload(crcMismatch, "payload/model.bin");
    EXPECT_FALSE(validator().validateAndExtract(crcMismatch).hasValue());

    const auto signatureMismatch =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("real-avatar-model")}},
                        std::nullopt, std::nullopt, "test.release",
                        std::nullopt, true);
    EXPECT_FALSE(
        validator().validateAndExtract(signatureMismatch).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsUnknownKeyInvalidSignatureLengthAndInvalidKeyId) {
    const auto unknown =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "unknown.release");
    EXPECT_FALSE(validator().validateAndExtract(unknown).hasValue());

    const auto shortSignature =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "test.release", 63U);
    EXPECT_FALSE(
        validator().validateAndExtract(shortSignature).hasValue());

    const auto invalidId =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "test.release\n");
    EXPECT_FALSE(validator().validateAndExtract(invalidId).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDuplicateOrInvalidTrustedKeyConfiguration) {
    const auto package = writeSignedPack();
    auto duplicate = trustedKey();
    AvatarPackValidator duplicateKeys{
        {trustedKey(), std::move(duplicate)}, staging_};
    EXPECT_FALSE(duplicateKeys.validateAndExtract(package).hasValue());

    AvatarPackValidator invalidId{{trustedKey("bad key id")}, staging_};
    EXPECT_FALSE(invalidId.validateAndExtract(package).hasValue());

    auto invalidBytes = trustedKey();
    invalidBytes.publicKey.fill(std::byte{0});
    AvatarPackValidator invalidKey{{std::move(invalidBytes)}, staging_};
    EXPECT_FALSE(invalidKey.validateAndExtract(package).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, RejectsMissingAndExtraPayloadEntries) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    const auto missing =
        writeSignedPack(payloads, std::nullopt, std::nullopt,
                        "test.release", std::nullopt, false, {},
                        "payload/model.bin");
    EXPECT_FALSE(validator().validateAndExtract(missing).hasValue());

    const auto extra =
        writeSignedPack(payloads, std::nullopt, std::nullopt,
                        "test.release", std::nullopt, false,
                        {{.path = "payload/extra.bin",
                          .contents = bytes("extra")}});
    EXPECT_FALSE(validator().validateAndExtract(extra).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsManifestHashAndDeclaredPayloadByteMismatch) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    std::string wrongDigest(64U, '0');
    const auto wrongHash =
        writeSignedPack(payloads, std::nullopt, wrongDigest);
    EXPECT_FALSE(validator().validateAndExtract(wrongHash).hasValue());

    const auto wrongBytes =
        writeSignedPack(payloads, payloads.front().contents.size() + 1U);
    EXPECT_FALSE(validator().validateAndExtract(wrongBytes).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, CleansStagingAfterExtractionWriteFailure) {
    const std::string overlongComponent(300U, 'x');
    const auto package =
        writeSignedPack("payload/" + overlongComponent, "model");

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::IoFailure);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       AppliesArchiveValidationBeforeSignatureValidation) {
    auto package = writeSignedPack(
        {{.path = "payload/model.bin", .contents = bytes("model")}},
        std::nullopt, std::nullopt, "test.release", std::nullopt, true);
    patchNameByte(package, "payload/model.bin", 7U,
                  static_cast<std::uint8_t>('\\'));

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    ASSERT_TRUE(result.error().issueCode().has_value());
    EXPECT_EQ(*result.error().issueCode(), "avatar.pack.archive.path");
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsMetadataSizeAndAggregateExpansionPastTwoGiB) {
    const auto longKey =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, std::string(129U, 'a'));
    EXPECT_FALSE(validator().validateAndExtract(longKey).hasValue());

    auto boundary =
        writeArchive({{.path = "payload/0", .contents = bytes("x")},
                      {.path = "payload/1", .contents = bytes("x")},
                      {.path = "payload/2", .contents = bytes("x")},
                      {.path = "payload/3", .contents = bytes("x")},
                      {.path = "payload/4", .contents = bytes("x")}});
    for (std::size_t index = 0; index < 4U; ++index) {
        patchSizes(boundary, "payload/" + std::to_string(index),
                   std::nullopt,
                   static_cast<std::uint32_t>(512ULL * kMiB));
    }
    patchSizes(boundary, "payload/4", std::nullopt, 1U);
    EXPECT_FALSE(validator().validateAndExtract(boundary).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

class AvatarPackValidatorFuzzSeedTest : public AvatarPackValidatorTest {};

TEST_F(AvatarPackValidatorFuzzSeedTest,
       RunsDeterministicMalformedArchiveSeedLoop) {
    const auto valid = readBytes(writeSignedPack());
    ASSERT_GT(valid.size(), 32U);
    for (std::size_t seed = 1U; seed <= 32U; ++seed) {
        auto mutated = valid;
        mutated.resize(mutated.size() - seed);
        const auto path = uniquePack();
        writeBytes(path, mutated);
        const auto result = validator().validateAndExtract(path);
        EXPECT_FALSE(result.hasValue()) << "seed " << seed;
        EXPECT_TRUE(directoryEmpty(staging_)) << "seed " << seed;
    }
}

}  // namespace
}  // namespace creator::avatar_pack_adapter
