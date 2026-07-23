#include "avatar/AvatarAssetManifestCodec.h"

#include "avatar/AvatarAssetSchema.h"
#include "core/AppError.h"
#include "core/Uuid.h"

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace creator::avatar {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;
using nlohmann::json;

constexpr std::uintmax_t kMaximumFileSize = 8U * 1024U * 1024U;

AppError codecError(ErrorCode code, std::string message, std::string issueCode,
                    std::string messageKey) {
    return {code, std::move(message), std::move(issueCode), std::move(messageKey)};
}

class SchemaErrorHandler final
    : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const json::json_pointer& pointer, const json& instance,
               const std::string& message) override {
        basic_error_handler::error(pointer, instance, message);
        if (!failed_) {
            failed_ = true;
            pointer_ = pointer.to_string();
            message_ = message;
        }
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& pointer() const noexcept { return pointer_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    bool failed_{false};
    std::string pointer_;
    std::string message_;
};

const nlohmann::json_schema::json_validator& validator() {
    static const nlohmann::json_schema::json_validator instance = [] {
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        auto schema = json::parse(embedded::kAvatarAssetSchema);
        // This validator release narrows unsigned schema limits through int64.
        // Keep the exact uint64 limits in the committed schema and enforce
        // those two native-width fields explicitly before decoding instead.
        auto& performance =
            schema["definitions"]["performance"]["properties"];
        for (const auto* field : {"payloadBytes", "textureBytes"}) {
            performance[field].erase("minimum");
            performance[field].erase("maximum");
        }
        compiled.set_root_schema(schema);
        return compiled;
    }();
    return instance;
}

Result<void> validate(const json& document) {
    try {
        SchemaErrorHandler errors;
        validator().validate(document, errors);
        if (errors.failed()) {
            return codecError(
                ErrorCode::ParseFailure,
                "avatar asset schema violation at '" + errors.pointer() +
                    "': " + errors.message(),
                "avatar.asset.codec.schema", "avatar.validation.schema");
        }
        return core::ok();
    } catch (const std::exception& error) {
        return codecError(
            ErrorCode::ParseFailure,
            "avatar asset schema could not be compiled: " +
                std::string{error.what()},
            "avatar.asset.codec.schema", "avatar.validation.schema");
    }
}

template <typename Unsigned>
bool fitsUnsignedNativeWidth(const json& value) {
    static_assert(std::is_unsigned_v<Unsigned>);
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(
                   std::numeric_limits<Unsigned>::max());
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        return signedValue >= 0 &&
               static_cast<std::uint64_t>(signedValue) <=
                   static_cast<std::uint64_t>(
                       std::numeric_limits<Unsigned>::max());
    }
    return false;
}

Result<void> validatePerformanceNativeWidths(const json& document) {
    const auto& performance = document.at("performance");
    const bool valid =
        fitsUnsignedNativeWidth<std::uint64_t>(
            performance.at("payloadBytes")) &&
        fitsUnsignedNativeWidth<std::uint64_t>(
            performance.at("textureBytes")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("textureCount")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("maxTextureDimension")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("vertexCount")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("triangleCount")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("drawCallCount")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("drawPartCount")) &&
        fitsUnsignedNativeWidth<std::uint32_t>(
            performance.at("boneCount"));
    if (!valid) {
        return codecError(
            ErrorCode::ParseFailure,
            "avatar asset performance metadata exceeds its native width",
            "avatar.asset.codec.performance-width",
            "avatar.validation.schema");
    }
    return core::ok();
}

template <typename Enum, std::size_t Size>
std::string_view enumToString(
    Enum value, const std::array<std::pair<Enum, std::string_view>, Size>& values) {
    const auto found =
        std::find_if(values.begin(), values.end(),
                     [value](const auto& candidate) {
                         return candidate.first == value;
                     });
    return found == values.end() ? std::string_view{} : found->second;
}

template <typename Enum, std::size_t Size>
std::optional<Enum> enumFromString(
    std::string_view value,
    const std::array<std::pair<Enum, std::string_view>, Size>& values) {
    const auto found =
        std::find_if(values.begin(), values.end(),
                     [value](const auto& candidate) {
                         return candidate.second == value;
                     });
    if (found == values.end()) return std::nullopt;
    return found->first;
}

constexpr std::array<std::pair<AvatarRepresentation, std::string_view>, 3>
    kRepresentations{{
        {AvatarRepresentation::Inochi2d, "inochi2d"},
        {AvatarRepresentation::Vrm1, "vrm-1.0"},
        {AvatarRepresentation::GltfRig, "gltf-rig"},
    }};
constexpr std::array<std::pair<RigFamily, std::string_view>, 6> kRigFamilies{{
    {RigFamily::Humanoid, "humanoid"},
    {RigFamily::Kemonomimi, "kemonomimi"},
    {RigFamily::AnthroBiped, "anthro-biped"},
    {RigFamily::Mascot, "mascot"},
    {RigFamily::Quadruped, "quadruped"},
    {RigFamily::Avian, "avian"},
}};
constexpr std::array<std::pair<AvatarSlot, std::string_view>, 31> kSlots{{
    {AvatarSlot::Body, "body"},
    {AvatarSlot::Head, "head"},
    {AvatarSlot::Face, "face"},
    {AvatarSlot::Skin, "skin"},
    {AvatarSlot::HairFront, "hair-front"},
    {AvatarSlot::HairSideLeft, "hair-side-left"},
    {AvatarSlot::HairSideRight, "hair-side-right"},
    {AvatarSlot::HairBack, "hair-back"},
    {AvatarSlot::HairTie, "hair-tie"},
    {AvatarSlot::Brows, "brows"},
    {AvatarSlot::Eyes, "eyes"},
    {AvatarSlot::Nose, "nose"},
    {AvatarSlot::Mouth, "mouth"},
    {AvatarSlot::Teeth, "teeth"},
    {AvatarSlot::EarLeft, "ear-left"},
    {AvatarSlot::EarRight, "ear-right"},
    {AvatarSlot::Muzzle, "muzzle"},
    {AvatarSlot::HornLeft, "horn-left"},
    {AvatarSlot::HornRight, "horn-right"},
    {AvatarSlot::WingLeft, "wing-left"},
    {AvatarSlot::WingRight, "wing-right"},
    {AvatarSlot::Tail, "tail"},
    {AvatarSlot::Inner, "inner"},
    {AvatarSlot::Top, "top"},
    {AvatarSlot::Bottom, "bottom"},
    {AvatarSlot::Outerwear, "outerwear"},
    {AvatarSlot::Hands, "hands"},
    {AvatarSlot::Footwear, "footwear"},
    {AvatarSlot::Headwear, "headwear"},
    {AvatarSlot::FaceAccessory, "face-accessory"},
    {AvatarSlot::BodyAccessory, "body-accessory"},
}};
constexpr std::array<std::pair<AvatarRight, std::string_view>, 7> kRights{{
    {AvatarRight::CommercialBroadcast, "commercial-broadcast"},
    {AvatarRight::AppBundle, "app-bundle"},
    {AvatarRight::DerivativeCharacter, "derivative-character"},
    {AvatarRight::ModelExport, "model-export"},
    {AvatarRight::RawAssetRedistribution, "raw-asset-redistribution"},
    {AvatarRight::PortableProject, "portable-project"},
    {AvatarRight::Attribution, "attribution"},
}};
constexpr std::array<std::pair<GrantState, std::string_view>, 4> kGrantStates{{
    {GrantState::Allowed, "allowed"},
    {GrantState::Denied, "denied"},
    {GrantState::Conditional, "conditional"},
    {GrantState::Unknown, "unknown"},
}};

template <typename Enum, std::size_t Size>
json enumArray(std::vector<Enum> values,
               const std::array<std::pair<Enum, std::string_view>, Size>& names) {
    std::sort(values.begin(), values.end());
    json result = json::array();
    for (const auto value : values) result.push_back(enumToString(value, names));
    return result;
}

bool containsTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(),
                       [](const std::filesystem::path& component) {
                           return component == "..";
                       });
}

Result<void> ensureSafePath(const std::filesystem::path& path) {
    if (path.empty() || containsTraversal(path)) {
        return codecError(ErrorCode::InvalidArgument,
                          "avatar asset path must not contain traversal",
                          "avatar.asset.codec.path", "avatar.validation.path");
    }
    return core::ok();
}

std::filesystem::path temporaryPathFor(const std::filesystem::path& target) {
    std::filesystem::path filename{"."};
    filename += target.filename();
    filename += ".part-";
    filename += core::generateUuidV4();
    return target.parent_path() / filename;
}

void removeTemporary(const std::filesystem::path& temporary) noexcept {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

AppError durableError(std::string_view operation, std::uint64_t code) {
    return codecError(
        ErrorCode::IoFailure,
        "avatar asset durable file " + std::string{operation} +
            " failed (code " + std::to_string(code) + ")",
        "avatar.asset.codec.save", "avatar.validation.io");
}

Result<void> writeAtomically(const std::filesystem::path& target,
                             std::string_view contents) {
    std::filesystem::path temporary;
    try {
        temporary = temporaryPathFor(target);
    } catch (const std::exception&) {
        return codecError(ErrorCode::IoFailure,
                          "avatar asset temporary path is invalid",
                          "avatar.asset.codec.save", "avatar.validation.io");
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return durableError("create temporary", GetLastError());
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto remaining = contents.size() - written;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining,
                                  std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        const BOOL succeeded =
            WriteFile(handle, contents.data() + written, chunk, &count, nullptr);
        if (!succeeded || count == 0) {
            const DWORD code = succeeded ? ERROR_WRITE_FAULT : GetLastError();
            CloseHandle(handle);
            removeTemporary(temporary);
            return durableError("write temporary", code);
        }
        written += count;
    }
    if (!FlushFileBuffers(handle)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        removeTemporary(temporary);
        return durableError("flush temporary", code);
    }
    if (!CloseHandle(handle)) {
        const DWORD code = GetLastError();
        removeTemporary(temporary);
        return durableError("close temporary", code);
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        removeTemporary(temporary);
        return durableError("replace target", code);
    }
#else
    const int descriptor =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0)
        return durableError("create temporary",
                            static_cast<std::uint64_t>(errno));
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto remaining = contents.size() - written;
        const auto chunk = std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count =
            ::write(descriptor, contents.data() + written, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int code = errno;
            ::close(descriptor);
            removeTemporary(temporary);
            return durableError("write temporary",
                                static_cast<std::uint64_t>(code));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const int code = errno;
        ::close(descriptor);
        removeTemporary(temporary);
        return durableError("flush temporary",
                            static_cast<std::uint64_t>(code));
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("close temporary",
                            static_cast<std::uint64_t>(code));
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("replace target",
                            static_cast<std::uint64_t>(code));
    }
    const auto parent = target.parent_path().empty()
                            ? std::filesystem::path{"."}
                            : target.parent_path();
    const int parentDescriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (parentDescriptor < 0)
        return durableError("open parent directory",
                            static_cast<std::uint64_t>(errno));
    if (::fsync(parentDescriptor) != 0) {
        const int code = errno;
        ::close(parentDescriptor);
        return durableError("flush parent directory",
                            static_cast<std::uint64_t>(code));
    }
    if (::close(parentDescriptor) != 0)
        return durableError("close parent directory",
                            static_cast<std::uint64_t>(errno));
#endif
    return core::ok();
}

}  // namespace

nlohmann::json AvatarAssetManifestCodec::toJson(
    const AvatarAssetManifest& manifest) const {
    const auto& values = manifest.values();
    auto dependencies = values.dependencies;
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.assetId.value(), left.version) <
                         std::tie(right.assetId.value(), right.version);
              });
    json dependencyJson = json::array();
    for (const auto& dependency : dependencies) {
        dependencyJson.push_back({{"assetId", dependency.assetId.value()},
                                  {"version", dependency.version}});
    }
    auto payloads = values.payloads;
    std::sort(payloads.begin(), payloads.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    json payloadJson = json::array();
    for (const auto& payload : payloads) {
        payloadJson.push_back(
            {{"path", payload.path}, {"sha256", payload.sha256}});
    }
    auto grants = values.grants;
    std::sort(grants.begin(), grants.end(),
              [](const LicenseGrant& left, const LicenseGrant& right) {
                  return left.right < right.right;
              });
    json grantJson = json::array();
    for (const auto& grant : grants) {
        grantJson.push_back(
            {{"right", enumToString(grant.right, kRights)},
             {"state", enumToString(grant.state, kGrantStates)},
             {"condition", grant.condition}});
    }
    auto regions = values.regionAllowList;
    std::sort(regions.begin(), regions.end());

    return {
        {"schemaVersion", manifest.schemaVersion()},
        {"packageId", values.packageId.value()},
        {"packageVersion", values.packageVersion},
        {"assetId", values.assetId.value()},
        {"assetVersion", values.assetVersion},
        {"displayName", values.displayName},
        {"vendor", values.vendor},
        {"supportedRepresentations",
         enumArray(values.supportedRepresentations, kRepresentations)},
        {"supportedRigFamilies",
         enumArray(values.supportedRigFamilies, kRigFamilies)},
        {"allowedSlots", enumArray(values.allowedSlots, kSlots)},
        {"dependencies", std::move(dependencyJson)},
        {"payloads", std::move(payloadJson)},
        {"performance",
         {{"payloadBytes", values.performance.payloadBytes},
          {"textureBytes", values.performance.textureBytes},
          {"textureCount", values.performance.textureCount},
          {"maxTextureDimension", values.performance.maxTextureDimension},
          {"vertexCount", values.performance.vertexCount},
          {"triangleCount", values.performance.triangleCount},
          {"drawCallCount", values.performance.drawCallCount},
          {"drawPartCount", values.performance.drawPartCount},
          {"boneCount", values.performance.boneCount}}},
        {"sourceUri", values.sourceUri},
        {"licenseId", values.licenseId},
        {"licenseVersion", values.licenseVersion},
        {"grants", std::move(grantJson)},
        {"attributionText", values.attributionText},
        {"regionAllowList", std::move(regions)},
        {"validFrom", values.validFrom.toRfc3339()},
        {"validUntil",
         values.validUntil.has_value()
             ? json(values.validUntil->toRfc3339())
             : json(nullptr)},
    };
}

core::Result<AvatarAssetManifest> AvatarAssetManifestCodec::fromJson(
    const nlohmann::json& document) const {
    if (document.is_object() && document.contains("schemaVersion")) {
        const auto& version = document.at("schemaVersion");
        if ((version.is_number_unsigned() &&
             version.get<std::uint64_t>() >
                 static_cast<std::uint64_t>(
                     AvatarAssetManifest::kCurrentSchemaVersion)) ||
            (version.is_number_integer() &&
             version.get<std::int64_t>() >
                 static_cast<std::int64_t>(
                     AvatarAssetManifest::kCurrentSchemaVersion))) {
            return codecError(ErrorCode::UnsupportedVersion,
                              "avatar asset schema version is unsupported",
                              "avatar.asset.codec.version",
                              "avatar.validation.schema-version");
        }
    }
    if (auto valid = validate(document); !valid.hasValue()) return valid.error();
    if (auto widths = validatePerformanceNativeWidths(document);
        !widths.hasValue()) {
        return widths.error();
    }
    const auto& version = document.at("schemaVersion");
    const bool current =
        (version.is_number_unsigned() &&
         version.get<std::uint64_t>() ==
             static_cast<std::uint64_t>(
                 AvatarAssetManifest::kCurrentSchemaVersion)) ||
        (version.is_number_integer() &&
         version.get<std::int64_t>() ==
             static_cast<std::int64_t>(
                 AvatarAssetManifest::kCurrentSchemaVersion));
    if (!current) {
        return codecError(ErrorCode::UnsupportedVersion,
                          "avatar asset schema version is unsupported",
                          "avatar.asset.codec.version",
                          "avatar.validation.schema-version");
    }

    try {
        auto packageId =
            AvatarPackageId::create(document.at("packageId").get<std::string>());
        auto assetId =
            AvatarAssetId::create(document.at("assetId").get<std::string>());
        if (!packageId.hasValue() || !assetId.hasValue()) {
            return codecError(ErrorCode::ParseFailure,
                              "avatar asset contains an invalid identifier",
                              "avatar.asset.codec.identifier",
                              "avatar.validation.schema");
        }
        const auto validFrom = core::Utc::parseRfc3339(
            document.at("validFrom").get<std::string>());
        if (!validFrom.hasValue()) {
            return codecError(ErrorCode::ParseFailure,
                              "avatar asset contains invalid valid-from",
                              "avatar.asset.codec.time",
                              "avatar.validation.schema");
        }
        std::optional<core::Utc> validUntil;
        if (!document.at("validUntil").is_null()) {
            const auto parsed = core::Utc::parseRfc3339(
                document.at("validUntil").get<std::string>());
            if (!parsed.hasValue()) {
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains invalid valid-until",
                                  "avatar.asset.codec.time",
                                  "avatar.validation.schema");
            }
            validUntil = parsed.value();
        }
        AvatarAssetManifestDraft draft{
            .packageId = packageId.value(),
            .packageVersion =
                document.at("packageVersion").get<std::string>(),
            .assetId = assetId.value(),
            .assetVersion = document.at("assetVersion").get<std::string>(),
            .displayName = document.at("displayName").get<std::string>(),
            .vendor = document.at("vendor").get<std::string>(),
            .performance =
                {.payloadBytes =
                     document.at("performance")
                         .at("payloadBytes")
                         .get<std::uint64_t>(),
                 .textureBytes =
                     document.at("performance")
                         .at("textureBytes")
                         .get<std::uint64_t>(),
                 .textureCount =
                     document.at("performance")
                         .at("textureCount")
                         .get<std::uint32_t>(),
                 .maxTextureDimension =
                     document.at("performance")
                         .at("maxTextureDimension")
                         .get<std::uint32_t>(),
                 .vertexCount =
                     document.at("performance")
                         .at("vertexCount")
                         .get<std::uint32_t>(),
                 .triangleCount =
                     document.at("performance")
                         .at("triangleCount")
                         .get<std::uint32_t>(),
                 .drawCallCount =
                     document.at("performance")
                         .at("drawCallCount")
                         .get<std::uint32_t>(),
                 .drawPartCount =
                     document.at("performance")
                         .at("drawPartCount")
                         .get<std::uint32_t>(),
                 .boneCount =
                     document.at("performance")
                         .at("boneCount")
                         .get<std::uint32_t>()},
            .sourceUri = document.at("sourceUri").get<std::string>(),
            .licenseId = document.at("licenseId").get<std::string>(),
            .licenseVersion =
                document.at("licenseVersion").get<std::string>(),
            .attributionText =
                document.at("attributionText").get<std::string>(),
            .regionAllowList =
                document.at("regionAllowList")
                    .get<std::vector<std::string>>(),
            .validFrom = validFrom.value(),
            .validUntil = validUntil,
        };
        for (const auto& value : document.at("supportedRepresentations")) {
            const auto parsed = enumFromString(
                value.get<std::string>(), kRepresentations);
            if (!parsed.has_value())
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains an invalid representation",
                                  "avatar.asset.codec.enum",
                                  "avatar.validation.schema");
            draft.supportedRepresentations.push_back(*parsed);
        }
        for (const auto& value : document.at("supportedRigFamilies")) {
            const auto parsed =
                enumFromString(value.get<std::string>(), kRigFamilies);
            if (!parsed.has_value())
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains an invalid rig family",
                                  "avatar.asset.codec.enum",
                                  "avatar.validation.schema");
            draft.supportedRigFamilies.push_back(*parsed);
        }
        for (const auto& value : document.at("allowedSlots")) {
            const auto parsed =
                enumFromString(value.get<std::string>(), kSlots);
            if (!parsed.has_value())
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains an invalid slot",
                                  "avatar.asset.codec.enum",
                                  "avatar.validation.schema");
            draft.allowedSlots.push_back(*parsed);
        }
        for (const auto& value : document.at("dependencies")) {
            auto dependencyId =
                AvatarAssetId::create(value.at("assetId").get<std::string>());
            if (!dependencyId.hasValue())
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains an invalid dependency",
                                  "avatar.asset.codec.identifier",
                                  "avatar.validation.schema");
            draft.dependencies.push_back(
                {.assetId = dependencyId.value(),
                 .version = value.at("version").get<std::string>()});
        }
        for (const auto& value : document.at("payloads")) {
            draft.payloads.push_back(
                {.path = value.at("path").get<std::string>(),
                 .sha256 = value.at("sha256").get<std::string>()});
        }
        for (const auto& value : document.at("grants")) {
            const auto right =
                enumFromString(value.at("right").get<std::string>(), kRights);
            const auto state = enumFromString(
                value.at("state").get<std::string>(), kGrantStates);
            if (!right.has_value() || !state.has_value())
                return codecError(ErrorCode::ParseFailure,
                                  "avatar asset contains an invalid grant",
                                  "avatar.asset.codec.enum",
                                  "avatar.validation.schema");
            draft.grants.push_back(
                {.right = *right,
                 .state = *state,
                 .condition =
                     value.at("condition").get<std::string>()});
        }
        auto result = AvatarAssetManifest::create(std::move(draft));
        if (!result.hasValue()) {
            return codecError(ErrorCode::ParseFailure,
                              "avatar asset failed domain validation: " +
                                  result.error().message(),
                              "avatar.asset.codec.domain",
                              "avatar.validation.schema");
        }
        return result;
    } catch (const std::exception& error) {
        return codecError(
            ErrorCode::ParseFailure,
            "avatar asset could not be decoded: " + std::string{error.what()},
            "avatar.asset.codec.decode", "avatar.validation.schema");
    }
}

core::Result<AvatarAssetManifest> AvatarAssetManifestCodec::load(
    const std::filesystem::path& path) const {
    if (auto safe = ensureSafePath(path); !safe.hasValue()) return safe.error();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error)
            return codecError(ErrorCode::IoFailure,
                              "avatar asset path could not be inspected",
                              "avatar.asset.codec.load",
                              "avatar.validation.io");
        return codecError(ErrorCode::NotFound,
                          "avatar asset file does not exist",
                          "avatar.asset.codec.load",
                          "avatar.validation.not-found");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return codecError(ErrorCode::IoFailure,
                          "avatar asset file size could not be read",
                          "avatar.asset.codec.load", "avatar.validation.io");
    if (size > kMaximumFileSize)
        return codecError(ErrorCode::ParseFailure,
                          "avatar asset file exceeds 8 MiB",
                          "avatar.asset.codec.load", "avatar.validation.size");
    std::ifstream stream{path, std::ios::binary};
    if (!stream)
        return codecError(ErrorCode::IoFailure,
                          "avatar asset file could not be opened",
                          "avatar.asset.codec.load", "avatar.validation.io");
    std::string contents(static_cast<std::size_t>(size), '\0');
    stream.read(contents.data(),
                static_cast<std::streamsize>(contents.size()));
    if (!stream && !stream.eof())
        return codecError(ErrorCode::IoFailure,
                          "avatar asset file could not be read",
                          "avatar.asset.codec.load", "avatar.validation.io");
    try {
        bool duplicateMember = false;
        std::vector<std::unordered_set<std::string>> objectMembers;
        const auto callback =
            [&duplicateMember, &objectMembers](
                int, json::parse_event_t event, json& parsed) {
                switch (event) {
                case json::parse_event_t::object_start:
                    objectMembers.emplace_back();
                    break;
                case json::parse_event_t::key:
                    if (!objectMembers.back()
                             .insert(parsed.get<std::string>())
                             .second) {
                        duplicateMember = true;
                    }
                    break;
                case json::parse_event_t::object_end:
                    objectMembers.pop_back();
                    break;
                default: break;
                }
                return true;
            };
        auto document = json::parse(contents, callback);
        if (duplicateMember) {
            return codecError(
                ErrorCode::ParseFailure,
                "avatar asset JSON contains a duplicate object member",
                "avatar.asset.codec.duplicate-member",
                "avatar.validation.json");
        }
        return fromJson(document);
    } catch (const std::exception& error) {
        return codecError(
            ErrorCode::ParseFailure,
            "avatar asset JSON could not be parsed: " +
                std::string{error.what()},
            "avatar.asset.codec.parse", "avatar.validation.json");
    }
}

core::Result<void> AvatarAssetManifestCodec::save(
    const std::filesystem::path& path,
    const AvatarAssetManifest& manifest) const {
    if (auto safe = ensureSafePath(path); !safe.hasValue()) return safe.error();
    try {
        const auto contents = toJson(manifest).dump(2);
        if (contents.size() > kMaximumFileSize) {
            return codecError(ErrorCode::ParseFailure,
                              "avatar asset file exceeds 8 MiB",
                              "avatar.asset.codec.save",
                              "avatar.validation.size");
        }
        return writeAtomically(path, contents);
    } catch (const std::exception& error) {
        return codecError(
            ErrorCode::ParseFailure,
            "avatar asset JSON could not be serialized: " +
                std::string{error.what()},
            "avatar.asset.codec.serialize", "avatar.validation.json");
    }
}

}  // namespace creator::avatar
