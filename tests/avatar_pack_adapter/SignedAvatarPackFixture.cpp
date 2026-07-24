#include "SignedAvatarPackFixture.h"

#include "avatar/AvatarAssetManifestCodec.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <stdexcept>
#include <utility>

namespace creator::avatar_pack_adapter::test {
namespace {

using avatar::AvatarAssetManifest;
using avatar::AvatarAssetManifestCodec;
using avatar::AvatarAssetManifestDraft;
using avatar::AvatarPayloadHash;
using avatar::AvatarRepresentation;
using avatar::AvatarRight;
using avatar::AvatarSlot;
using avatar::GrantState;
using avatar::RigFamily;

struct ZipEntry final {
    std::string path;
    std::vector<std::uint8_t> contents;
};

void append16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

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
    if (sodium_hex2bin(decoded.data(), decoded.size(), value.data(),
                       value.size(), nullptr, &decodedLength, nullptr) != 0 ||
        decodedLength != decoded.size()) {
        throw std::runtime_error{"test digest could not be decoded"};
    }
    return decoded;
}

AvatarAssetManifest manifestFor(const SignedPackOptions& options) {
    std::vector<AvatarPayloadHash> hashes;
    std::uint64_t payloadBytes = 0;
    for (const auto& payload : options.payloads) {
        payloadBytes += payload.contents.size();
        hashes.push_back(
            {.path = payload.path, .sha256 = digest(payload.contents)});
    }
    auto packageId = avatar::AvatarPackageId::create(options.packageId);
    auto assetId = avatar::AvatarAssetId::create(options.assetId);
    const auto validFrom =
        core::Utc::parseRfc3339("2026-07-24T00:00:00Z");
    if (!packageId.hasValue() || !assetId.hasValue() ||
        !validFrom.hasValue()) {
        throw std::runtime_error{"test manifest identity is invalid"};
    }
    AvatarAssetManifestDraft draft{
        .packageId = packageId.value(),
        .packageVersion = options.packageVersion,
        .assetId = assetId.value(),
        .assetVersion = options.assetVersion,
        .displayName = "Foundation Body",
        .vendor = "Creator Studio",
        .supportedRepresentations = {AvatarRepresentation::GltfRig},
        .supportedRigFamilies = {RigFamily::Humanoid},
        .allowedSlots = {AvatarSlot::Body},
        .dependencies = {},
        .payloads = std::move(hashes),
        .performance =
            {.payloadBytes =
                 options.declaredPayloadBytes.value_or(payloadBytes),
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

void writeStoredZip(const std::filesystem::path& path,
                    const std::vector<ZipEntry>& entries) {
    struct CentralRecord final {
        const ZipEntry* entry{};
        std::uint32_t crc{};
        std::uint32_t localOffset{};
    };

    std::vector<std::uint8_t> archive;
    std::vector<CentralRecord> central;
    for (const auto& entry : entries) {
        if (entry.path.size() > UINT16_MAX ||
            entry.contents.size() > UINT32_MAX ||
            archive.size() > UINT32_MAX) {
            throw std::runtime_error{"test ZIP input is too large"};
        }
        const auto crc = static_cast<std::uint32_t>(
            mz_crc32(MZ_CRC32_INIT, entry.contents.data(),
                     entry.contents.size()));
        central.push_back(
            {.entry = &entry,
             .crc = crc,
             .localOffset = static_cast<std::uint32_t>(archive.size())});
        append32(archive, 0x04034b50U);
        append16(archive, 20U);
        append16(archive, 0x0800U);
        append16(archive, 0U);
        append16(archive, 0U);
        append16(archive, 0U);
        append32(archive, crc);
        append32(archive,
                 static_cast<std::uint32_t>(entry.contents.size()));
        append32(archive,
                 static_cast<std::uint32_t>(entry.contents.size()));
        append16(archive, static_cast<std::uint16_t>(entry.path.size()));
        append16(archive, 0U);
        archive.insert(archive.end(), entry.path.begin(), entry.path.end());
        archive.insert(archive.end(), entry.contents.begin(),
                       entry.contents.end());
    }

    const auto centralOffset = static_cast<std::uint32_t>(archive.size());
    for (const auto& record : central) {
        const auto& entry = *record.entry;
        append32(archive, 0x02014b50U);
        append16(archive, 0x0314U);
        append16(archive, 20U);
        append16(archive, 0x0800U);
        append16(archive, 0U);
        append16(archive, 0U);
        append16(archive, 0U);
        append32(archive, record.crc);
        append32(archive,
                 static_cast<std::uint32_t>(entry.contents.size()));
        append32(archive,
                 static_cast<std::uint32_t>(entry.contents.size()));
        append16(archive, static_cast<std::uint16_t>(entry.path.size()));
        append16(archive, 0U);
        append16(archive, 0U);
        append16(archive, 0U);
        append16(archive, 0U);
        append32(archive, 0100600U << 16U);
        append32(archive, record.localOffset);
        archive.insert(archive.end(), entry.path.begin(), entry.path.end());
    }
    const auto centralBytes =
        static_cast<std::uint32_t>(archive.size()) - centralOffset;
    append32(archive, 0x06054b50U);
    append16(archive, 0U);
    append16(archive, 0U);
    append16(archive, static_cast<std::uint16_t>(central.size()));
    append16(archive, static_cast<std::uint16_t>(central.size()));
    append32(archive, centralBytes);
    append32(archive, centralOffset);
    append16(archive, 0U);

    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error{"test ZIP could not be created"};
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
    output.close();
    if (!output) throw std::runtime_error{"test ZIP could not be written"};
}

}  // namespace

SignedAvatarPackFixture::SignedAvatarPackFixture(
    std::filesystem::path workspace)
    : workspace_(std::move(workspace)) {
    if (sodium_init() < 0 ||
        crypto_sign_keypair(publicKey_.data(), secretKey_.data()) != 0) {
        throw std::runtime_error{"libsodium fixture initialization failed"};
    }
    std::filesystem::create_directories(workspace_);
}

TrustedAvatarKey SignedAvatarPackFixture::trustedKey() const {
    TrustedAvatarKey key{.keyId = "test.release"};
    std::transform(publicKey_.begin(), publicKey_.end(),
                   key.publicKey.begin(), [](std::uint8_t value) {
                       return static_cast<std::byte>(value);
                   });
    return key;
}

std::filesystem::path SignedAvatarPackFixture::writePack(
    SignedPackOptions options) {
    if (options.payloads.empty()) {
        options.payloads.push_back(
            {.path = "payload/model.bin",
             .contents = bytes("real-avatar-model")});
    }
    const auto manifest = manifestFor(options);
    const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
    const auto message = signatureMessage(manifest);
    std::array<std::uint8_t, crypto_sign_BYTES> signature{};
    unsigned long long signatureBytes = 0;
    if (crypto_sign_detached(signature.data(), &signatureBytes,
                             message.data(), message.size(),
                             secretKey_.data()) != 0 ||
        signatureBytes != signature.size()) {
        throw std::runtime_error{"libsodium could not sign test pack"};
    }
    if (options.corruptSignature) signature[0] ^= 0x80U;

    std::vector<ZipEntry> entries{
        {.path = "manifest.json", .contents = bytes(canonical)},
        {.path = "signature.ed25519",
         .contents = {signature.begin(), signature.end()}},
        {.path = "signing-key-id.txt",
         .contents = bytes("test.release")},
    };
    for (const auto& payload : options.payloads) {
        entries.push_back(
            {.path = payload.path, .contents = payload.contents});
    }
    const auto path =
        workspace_ / (core::generateUuidV4() + ".csavatarpack");
    writeStoredZip(path, entries);
    return path;
}

}  // namespace creator::avatar_pack_adapter::test
