#include "avatar_pack_adapter/AvatarPackValidator.h"

#include "avatar/AvatarAssetManifestCodec.h"
#include "avatar_pack_adapter/AvatarPackArchive.h"
#include "avatar_pack_adapter/AvatarPackStaging.h"
#include "avatar_pack_adapter/SodiumSignatureVerifier.h"
#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace creator::avatar_pack_adapter {
namespace {

using avatar::AvatarAssetManifest;
using avatar::AvatarAssetManifestCodec;
using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::size_t kMaximumManifestBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumKeyIdBytes = 128U;
constexpr std::string_view kManifestPath = "manifest.json";
constexpr std::string_view kSignaturePath = "signature.ed25519";
constexpr std::string_view kKeyIdPath = "signing-key-id.txt";

AppError packError(ErrorCode code, std::string message,
                   std::string issueCode, std::string messageKey) {
    return {code, std::move(message), std::move(issueCode),
            std::move(messageKey)};
}

AppError invalidPack(std::string message, std::string issueCode) {
    return packError(ErrorCode::InvalidArgument, std::move(message),
                     std::move(issueCode), "avatar.validation.pack");
}

AppError allocationError() {
    return packError(ErrorCode::InsufficientStorage,
                     "avatar pack validation allocation failed",
                     "avatar.pack.allocation", "avatar.validation.io");
}

AppError exceptionError() {
    return packError(ErrorCode::IoFailure,
                     "avatar pack validation failed safely",
                     "avatar.pack.exception", "avatar.validation.io");
}

bool validKeyId(std::string_view keyId) noexcept {
    return !keyId.empty() && keyId.size() <= kMaximumKeyIdBytes &&
           std::all_of(keyId.begin(), keyId.end(), [](unsigned char value) {
               return (value >= 'a' && value <= 'z') ||
                      (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '.' ||
                      value == '_' || value == '-';
           });
}

bool ordinalLess(std::string_view left, std::string_view right) {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(),
        [](unsigned char lhs, unsigned char rhs) { return lhs < rhs; });
}

std::optional<std::array<std::uint8_t, 32>> decodeHash(
    std::string_view value) {
    if (value.size() != 64U) return std::nullopt;
    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto decode = [](char character) -> std::optional<std::uint8_t> {
            if (character >= '0' && character <= '9')
                return static_cast<std::uint8_t>(character - '0');
            if (character >= 'a' && character <= 'f')
                return static_cast<std::uint8_t>(
                    character - 'a' + 10);
            return std::nullopt;
        };
        const auto high = decode(value[index * 2U]);
        const auto low = decode(value[index * 2U + 1U]);
        if (!high.has_value() || !low.has_value()) return std::nullopt;
        result[index] =
            static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return result;
}

Result<nlohmann::json> parseManifest(
    std::span<const std::uint8_t> raw) {
    try {
        bool duplicate = false;
        std::vector<std::unordered_set<std::string>> members;
        const auto callback =
            [&duplicate, &members](int, nlohmann::json::parse_event_t event,
                                   nlohmann::json& parsed) {
                switch (event) {
                case nlohmann::json::parse_event_t::object_start:
                    members.emplace_back();
                    break;
                case nlohmann::json::parse_event_t::key:
                    if (members.empty() ||
                        !members.back()
                             .insert(parsed.get<std::string>())
                             .second) {
                        duplicate = true;
                    }
                    break;
                case nlohmann::json::parse_event_t::object_end:
                    if (!members.empty()) members.pop_back();
                    break;
                default: break;
                }
                return true;
            };
        const auto text =
            std::string_view{reinterpret_cast<const char*>(raw.data()),
                             raw.size()};
        auto document = nlohmann::json::parse(text, callback);
        if (duplicate) {
            return invalidPack(
                "avatar pack manifest contains duplicate JSON members",
                "avatar.pack.manifest.duplicate-member");
        }
        return document;
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (const std::exception&) {
        return invalidPack("avatar pack manifest JSON is invalid",
                           "avatar.pack.manifest.parse");
    }
}

const AvatarPackArchiveEntry* findEntry(
    const std::vector<AvatarPackArchiveEntry>& entries,
    std::string_view path) {
    const auto found =
        std::find_if(entries.begin(), entries.end(),
                     [path](const auto& entry) {
                         return entry.path == path;
                     });
    return found == entries.end() ? nullptr : &*found;
}

bool metadataPath(std::string_view path) noexcept {
    return path == kManifestPath || path == kSignaturePath ||
           path == kKeyIdPath;
}

Result<const TrustedAvatarKey*> findTrustedKey(
    std::string_view keyId,
    const std::vector<TrustedAvatarKey>& trustedKeys,
    const SodiumSignatureVerifier& verifier) {
    if (!validKeyId(keyId)) {
        return invalidPack("avatar pack signing key ID is invalid",
                           "avatar.pack.signing-key.id");
    }
    std::unordered_set<std::string> ids;
    const TrustedAvatarKey* match = nullptr;
    for (const auto& key : trustedKeys) {
        if (!validKeyId(key.keyId)) {
            return invalidPack("avatar pack trusted key ID is invalid",
                               "avatar.pack.trusted-key.id");
        }
        if (!ids.insert(key.keyId).second) {
            return invalidPack(
                "avatar pack trusted key IDs must be unique",
                "avatar.pack.trusted-key.duplicate");
        }
        if (auto valid = verifier.validatePublicKey(key.publicKey);
            !valid.hasValue()) {
            return valid.error();
        }
        if (key.keyId == keyId) match = &key;
    }
    if (match == nullptr) {
        return invalidPack("avatar pack signing key is not trusted",
                           "avatar.pack.signing-key.unknown");
    }
    return match;
}

std::vector<std::uint8_t>
signatureMessage(const AvatarAssetManifest& manifest) {
    const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
    std::vector<std::uint8_t> message{canonical.begin(), canonical.end()};
    std::vector<const avatar::AvatarPayloadHash*> payloads;
    payloads.reserve(manifest.values().payloads.size());
    for (const auto& payload : manifest.values().payloads)
        payloads.push_back(&payload);
    std::sort(payloads.begin(), payloads.end(),
              [](const auto* left, const auto* right) {
                  return ordinalLess(left->path, right->path);
              });
    for (const auto* payload : payloads) {
        message.insert(message.end(), payload->path.begin(),
                       payload->path.end());
        message.push_back(0U);
        const auto hash = decodeHash(payload->sha256);
        if (!hash.has_value()) continue;
        message.insert(message.end(), hash->begin(), hash->end());
    }
    return message;
}

Result<void>
validatePayloadParity(const AvatarAssetManifest& manifest,
                      const std::vector<AvatarPackArchiveEntry>& entries) {
    std::vector<const AvatarPackArchiveEntry*> archivePayloads;
    for (const auto& entry : entries) {
        if (!metadataPath(entry.path)) archivePayloads.push_back(&entry);
    }
    std::vector<const avatar::AvatarPayloadHash*> declared;
    declared.reserve(manifest.values().payloads.size());
    for (const auto& payload : manifest.values().payloads)
        declared.push_back(&payload);
    std::sort(declared.begin(), declared.end(),
              [](const auto* left, const auto* right) {
                  return ordinalLess(left->path, right->path);
              });
    std::sort(archivePayloads.begin(), archivePayloads.end(),
              [](const auto* left, const auto* right) {
                  return ordinalLess(left->path, right->path);
              });
    if (archivePayloads.size() != declared.size()) {
        return invalidPack(
            "avatar pack payload entries do not match the manifest",
            "avatar.pack.payload.parity");
    }
    std::uint64_t actualBytes = 0;
    for (std::size_t index = 0; index < declared.size(); ++index) {
        if (archivePayloads[index]->path != declared[index]->path) {
            return invalidPack(
                "avatar pack payload entries do not match the manifest",
                "avatar.pack.payload.parity");
        }
        if (actualBytes > std::numeric_limits<std::uint64_t>::max() -
                              archivePayloads[index]->uncompressedBytes) {
            return invalidPack("avatar pack payload size overflowed",
                               "avatar.pack.payload.bytes");
        }
        actualBytes += archivePayloads[index]->uncompressedBytes;
    }
    if (actualBytes != manifest.values().performance.payloadBytes) {
        return invalidPack(
            "avatar pack declared payload bytes do not match the archive",
            "avatar.pack.payload.bytes");
    }
    return core::ok();
}

}  // namespace

AvatarPackValidator::AvatarPackValidator(
    std::vector<TrustedAvatarKey> trustedKeys,
    std::filesystem::path stagingParent)
    : trustedKeys_(std::move(trustedKeys)),
      stagingParent_(std::move(stagingParent)) {}

Result<ValidatedAvatarPack> AvatarPackValidator::validateAndExtract(
    const std::filesystem::path& packagePath) const noexcept {
    try {
        auto opened = AvatarPackArchive::open(packagePath);
        if (!opened.hasValue()) return opened.error();
        auto archive = std::move(opened).value();
        const auto& entries = archive.entries();

        const auto* manifestEntry = findEntry(entries, kManifestPath);
        const auto* signatureEntry = findEntry(entries, kSignaturePath);
        const auto* keyIdEntry = findEntry(entries, kKeyIdPath);
        if (manifestEntry == nullptr || signatureEntry == nullptr ||
            keyIdEntry == nullptr) {
            return invalidPack("avatar pack required metadata is missing",
                               "avatar.pack.metadata.required");
        }

        auto rawManifest = archive.read(*manifestEntry, kMaximumManifestBytes);
        if (!rawManifest.hasValue()) return rawManifest.error();
        auto rawSignature = archive.read(*signatureEntry, crypto_sign_BYTES);
        if (!rawSignature.hasValue()) return rawSignature.error();
        if (rawSignature.value().size() != crypto_sign_BYTES) {
            return invalidPack("avatar pack signature length is invalid",
                               "avatar.pack.signature.length");
        }
        auto rawKeyId = archive.read(*keyIdEntry, kMaximumKeyIdBytes);
        if (!rawKeyId.hasValue()) return rawKeyId.error();
        const std::string keyId{
            reinterpret_cast<const char*>(rawKeyId.value().data()),
            rawKeyId.value().size()};

        auto document = parseManifest(rawManifest.value());
        if (!document.hasValue()) return document.error();
        auto decoded = AvatarAssetManifestCodec{}.fromJson(document.value());
        if (!decoded.hasValue()) {
            return invalidPack("avatar pack manifest failed schema validation",
                               "avatar.pack.manifest.schema");
        }
        auto manifest = std::move(decoded).value();
        if (auto parity = validatePayloadParity(manifest, entries);
            !parity.hasValue()) {
            return parity.error();
        }

        const SodiumSignatureVerifier verifier;
        auto trusted = findTrustedKey(keyId, trustedKeys_, verifier);
        if (!trusted.hasValue()) return trusted.error();
        std::array<std::byte, crypto_sign_BYTES> signature{};
        std::transform(rawSignature.value().begin(), rawSignature.value().end(),
                       signature.begin(), [](std::uint8_t value) {
                           return static_cast<std::byte>(value);
                       });
        const auto message = signatureMessage(manifest);
        if (auto verified = verifier.verifyDetached(signature, message,
                                                    trusted.value()->publicKey);
            !verified.hasValue()) {
            return verified.error();
        }

        auto createdStaging = AvatarPackStaging::create(stagingParent_);
        if (!createdStaging.hasValue()) return createdStaging.error();
        auto staging = std::move(createdStaging).value();
        const auto failAfterStaging =
            [&staging](AppError original) -> Result<ValidatedAvatarPack> {
            auto cleaned = staging.cleanup();
            return cleaned.hasValue()
                       ? Result<ValidatedAvatarPack>{std::move(original)}
                       : Result<ValidatedAvatarPack>{cleaned.error()};
        };

        try {
            for (const auto& metadata : std::array{
                     std::pair{
                         kManifestPath,
                         std::span<const std::uint8_t>{rawManifest.value()}},
                     std::pair{
                         kSignaturePath,
                         std::span<const std::uint8_t>{rawSignature.value()}},
                     std::pair{kKeyIdPath, std::span<const std::uint8_t>{
                                               rawKeyId.value()}}}) {
                auto written =
                    staging.writeNewFile(metadata.first, metadata.second);
                if (!written.hasValue())
                    return failAfterStaging(written.error());
            }

            std::vector<const avatar::AvatarPayloadHash*> payloads;
            payloads.reserve(manifest.values().payloads.size());
            for (const auto& payload : manifest.values().payloads)
                payloads.push_back(&payload);
            std::sort(payloads.begin(), payloads.end(),
                      [](const auto* left, const auto* right) {
                          return ordinalLess(left->path, right->path);
                      });
            for (const auto* payload : payloads) {
                const auto* entry = findEntry(entries, payload->path);
                if (entry == nullptr) {
                    return failAfterStaging(invalidPack(
                        "avatar pack payload disappeared before extraction",
                        "avatar.pack.payload.parity"));
                }
                auto extracted = staging.extractNewFile(
                    archive, *entry, AvatarPackArchive::kMaximumEntryBytes);
                if (!extracted.hasValue())
                    return failAfterStaging(extracted.error());
                if (extracted.value() != payload->sha256) {
                    return failAfterStaging(
                        invalidPack("avatar pack payload hash is invalid",
                                    "avatar.pack.payload.hash"));
                }
            }

            auto sealed = staging.seal();
            if (!sealed.hasValue()) return failAfterStaging(sealed.error());
            return ValidatedAvatarPack{.manifest = std::move(manifest),
                                       .staging = std::move(staging)};
        } catch (const std::bad_alloc&) {
            return failAfterStaging(allocationError());
        } catch (...) {
            return failAfterStaging(exceptionError());
        }
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (...) {
        return exceptionError();
    }
}

}  // namespace creator::avatar_pack_adapter
