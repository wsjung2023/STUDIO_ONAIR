#include "SignedAvatarPackFixture.h"

#include "avatar/AvatarAssetManifestCodec.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <miniz.h>

#include <algorithm>
#include <array>
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

struct ZipReadView final {
    std::span<const std::uint8_t> contents;
};

std::size_t readZipEntry(void* opaque, mz_uint64 offset, void* output,
                         std::size_t requested) {
    const auto contents = static_cast<const ZipReadView*>(opaque)->contents;
    if (offset >= contents.size()) {
        return 0U;
    }
    const auto start = static_cast<std::size_t>(offset);
    const auto count = std::min(requested, contents.size() - start);
    std::copy_n(contents.data() + start, count,
                static_cast<std::uint8_t*>(output));
    return count;
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
    std::string_view canonical,
    const AvatarAssetManifest& manifest) {
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

void writeMinizZip(const std::filesystem::path& path,
                   const std::vector<ZipEntry>& entries) {
    mz_zip_archive archive{};
    mz_zip_zero_struct(&archive);
    const auto nativePath = path.string();
    if (!mz_zip_writer_init_file_v2(&archive, nativePath.c_str(), 0U, 0U)) {
        throw std::runtime_error{"miniz writer could not create test pack"};
    }

    bool writerOpen = true;
    try {
        // miniz otherwise records the wall clock. A fixed modification time
        // keeps same-seed/source acceptance archives byte-identical.
        const MZ_TIME_T fixedModified =
            static_cast<MZ_TIME_T>(1704067200);
        for (const auto& entry : entries) {
            ZipReadView view{entry.contents};
            constexpr mz_uint kWriterFlags =
                static_cast<mz_uint>(MZ_NO_COMPRESSION) |
                static_cast<mz_uint>(MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE);
            if (!mz_zip_writer_add_read_buf_callback(
                    &archive, entry.path.c_str(), readZipEntry,
                    &view, entry.contents.size(), &fixedModified, nullptr,
                    0U, kWriterFlags, nullptr, 0U, nullptr, 0U)) {
                throw std::runtime_error{
                    "miniz writer could not add test pack entry: " +
                    std::string{mz_zip_get_error_string(
                        mz_zip_get_last_error(&archive))}};
            }
        }
        if (!mz_zip_writer_finalize_archive(&archive)) {
            throw std::runtime_error{
                "miniz writer could not finalize test pack: " +
                std::string{mz_zip_get_error_string(
                    mz_zip_get_last_error(&archive))}};
        }
        const auto ended = mz_zip_writer_end(&archive);
        writerOpen = false;
        if (!ended) {
            throw std::runtime_error{
                "miniz writer could not close test pack"};
        }
    } catch (...) {
        if (writerOpen) {
            mz_zip_writer_end(&archive);
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }
}

void prepareFixtureWorkspace(const std::filesystem::path& workspace) {
    if (sodium_init() < 0) {
        throw std::runtime_error{"libsodium fixture initialization failed"};
    }
    std::filesystem::create_directories(workspace);
}

}  // namespace

SignedAvatarPackFixture::SignedAvatarPackFixture(
    std::filesystem::path workspace)
    : workspace_(std::move(workspace)) {
    prepareFixtureWorkspace(workspace_);
    if (crypto_sign_keypair(publicKey_.data(), secretKey_.data()) != 0) {
        throw std::runtime_error{"libsodium fixture initialization failed"};
    }
}

SignedAvatarPackFixture::SignedAvatarPackFixture(
    std::filesystem::path workspace,
    const std::array<std::uint8_t, crypto_sign_SEEDBYTES>& seed)
    : workspace_(std::move(workspace)) {
    prepareFixtureWorkspace(workspace_);
    if (crypto_sign_seed_keypair(publicKey_.data(), secretKey_.data(),
                                 seed.data()) != 0) {
        throw std::runtime_error{"libsodium fixture initialization failed"};
    }
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
    auto manifestDocument = AvatarAssetManifestCodec{}.toJson(manifest);
    if (options.rawManifestPackageVersion.has_value()) {
        manifestDocument["packageVersion"] =
            *options.rawManifestPackageVersion;
    }
    const auto canonical = manifestDocument.dump();
    const auto message = signatureMessage(canonical, manifest);
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
    writeMinizZip(path, entries);
    return path;
}

}  // namespace creator::avatar_pack_adapter::test
