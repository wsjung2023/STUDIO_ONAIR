#include "avatar/AvatarSpecCodec.h"

#include "avatar/AvatarSpecSchema.h"
#include "core/AppError.h"
#include "core/Uuid.h"

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
    return AppError{code, std::move(message), std::move(issueCode), std::move(messageKey)};
}

class SchemaErrorHandler final : public nlohmann::json_schema::basic_error_handler {
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

const nlohmann::json_schema::json_validator& avatarSpecValidator() {
    static const nlohmann::json_schema::json_validator validator = [] {
        nlohmann::json_schema::json_validator compiled{
            nullptr, nlohmann::json_schema::default_string_format_check};
        compiled.set_root_schema(json::parse(embedded::kAvatarSpecSchema));
        return compiled;
    }();
    return validator;
}

Result<void> validate(const json& document) {
    try {
        SchemaErrorHandler errors;
        avatarSpecValidator().validate(document, errors);
        if (errors.failed()) {
            return codecError(ErrorCode::ParseFailure,
                              "avatar spec schema violation at '" + errors.pointer() + "': " +
                                  errors.message(),
                              "avatar.spec.codec.schema", "avatar.validation.schema");
        }
        return core::ok();
    } catch (const std::exception& error) {
        return codecError(ErrorCode::ParseFailure,
                          "avatar spec schema could not be compiled: " +
                              std::string{error.what()},
                          "avatar.spec.codec.schema", "avatar.validation.schema");
    }
}

std::string toString(AvatarRepresentation representation) {
    switch (representation) {
    case AvatarRepresentation::Inochi2d: return "inochi2d";
    case AvatarRepresentation::Vrm1: return "vrm-1.0";
    case AvatarRepresentation::GltfRig: return "gltf-rig";
    }
    return {};
}

std::optional<AvatarRepresentation> representationFromString(std::string_view value) {
    if (value == "inochi2d") return AvatarRepresentation::Inochi2d;
    if (value == "vrm-1.0") return AvatarRepresentation::Vrm1;
    if (value == "gltf-rig") return AvatarRepresentation::GltfRig;
    return std::nullopt;
}

std::string toString(RigFamily rigFamily) {
    switch (rigFamily) {
    case RigFamily::Humanoid: return "humanoid";
    case RigFamily::Kemonomimi: return "kemonomimi";
    case RigFamily::AnthroBiped: return "anthro-biped";
    case RigFamily::Mascot: return "mascot";
    case RigFamily::Quadruped: return "quadruped";
    case RigFamily::Avian: return "avian";
    }
    return {};
}

std::optional<RigFamily> rigFamilyFromString(std::string_view value) {
    if (value == "humanoid") return RigFamily::Humanoid;
    if (value == "kemonomimi") return RigFamily::Kemonomimi;
    if (value == "anthro-biped") return RigFamily::AnthroBiped;
    if (value == "mascot") return RigFamily::Mascot;
    if (value == "quadruped") return RigFamily::Quadruped;
    if (value == "avian") return RigFamily::Avian;
    return std::nullopt;
}

constexpr std::array<std::pair<AvatarSlot, std::string_view>, 31> kSlots{{
    {AvatarSlot::Body, "body"}, {AvatarSlot::Head, "head"}, {AvatarSlot::Face, "face"},
    {AvatarSlot::Skin, "skin"}, {AvatarSlot::HairFront, "hair-front"},
    {AvatarSlot::HairSideLeft, "hair-side-left"}, {AvatarSlot::HairSideRight, "hair-side-right"},
    {AvatarSlot::HairBack, "hair-back"}, {AvatarSlot::HairTie, "hair-tie"},
    {AvatarSlot::Brows, "brows"}, {AvatarSlot::Eyes, "eyes"}, {AvatarSlot::Nose, "nose"},
    {AvatarSlot::Mouth, "mouth"}, {AvatarSlot::Teeth, "teeth"}, {AvatarSlot::EarLeft, "ear-left"},
    {AvatarSlot::EarRight, "ear-right"}, {AvatarSlot::Muzzle, "muzzle"},
    {AvatarSlot::HornLeft, "horn-left"}, {AvatarSlot::HornRight, "horn-right"},
    {AvatarSlot::WingLeft, "wing-left"}, {AvatarSlot::WingRight, "wing-right"},
    {AvatarSlot::Tail, "tail"}, {AvatarSlot::Inner, "inner"}, {AvatarSlot::Top, "top"},
    {AvatarSlot::Bottom, "bottom"}, {AvatarSlot::Outerwear, "outerwear"},
    {AvatarSlot::Hands, "hands"}, {AvatarSlot::Footwear, "footwear"},
    {AvatarSlot::Headwear, "headwear"}, {AvatarSlot::FaceAccessory, "face-accessory"},
    {AvatarSlot::BodyAccessory, "body-accessory"},
}};

std::string_view toString(AvatarSlot slot) {
    for (const auto& [candidate, spelling] : kSlots) {
        if (candidate == slot) return spelling;
    }
    return {};
}

std::optional<AvatarSlot> slotFromString(std::string_view value) {
    for (const auto& [slot, spelling] : kSlots) {
        if (spelling == value) return slot;
    }
    return std::nullopt;
}

json colorToJson(const ColorRgba& color) {
    return {{"red", color.red}, {"green", color.green}, {"blue", color.blue},
            {"alpha", color.alpha}};
}

ColorRgba colorFromJson(const json& value) {
    return {.red = value.at("red").get<float>(), .green = value.at("green").get<float>(),
            .blue = value.at("blue").get<float>(), .alpha = value.at("alpha").get<float>()};
}

json namedScalarsToJson(std::vector<NamedScalar> values) {
    std::sort(values.begin(), values.end(), [](const NamedScalar& left, const NamedScalar& right) {
        return left.name < right.name;
    });
    json result = json::array();
    for (const auto& value : values) result.push_back({{"name", value.name}, {"value", value.value}});
    return result;
}

std::vector<NamedScalar> namedScalarsFromJson(const json& values) {
    std::vector<NamedScalar> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back({.name = value.at("name").get<std::string>(),
                          .value = value.at("value").get<float>()});
    }
    return result;
}

bool containsTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const std::filesystem::path& part) {
        return part == "..";
    });
}

Result<void> ensureSafePath(const std::filesystem::path& path) {
    if (path.empty() || containsTraversal(path)) {
        return codecError(ErrorCode::InvalidArgument, "avatar spec path must not contain traversal",
                          "avatar.spec.codec.path", "avatar.validation.path");
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
    return codecError(ErrorCode::IoFailure,
                      "avatar spec durable file " + std::string{operation} + " failed (code " +
                          std::to_string(code) + ")",
                      "avatar.spec.codec.save", "avatar.validation.io");
}

Result<void> writeAtomically(const std::filesystem::path& target, std::string_view contents) {
    std::filesystem::path temporary;
    try {
        temporary = temporaryPathFor(target);
    } catch (const std::exception&) {
        return codecError(ErrorCode::IoFailure, "avatar spec temporary path is invalid",
                          "avatar.spec.codec.save", "avatar.validation.io");
    }

#ifdef _WIN32
    HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return durableError("create temporary", GetLastError());
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto remaining = contents.size() - written;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        const BOOL succeeded = WriteFile(handle, contents.data() + written, chunk, &count, nullptr);
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
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0) return durableError("create temporary", static_cast<std::uint64_t>(errno));
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto remaining = contents.size() - written;
        const auto chunk = std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t count = ::write(descriptor, contents.data() + written, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int code = errno;
            ::close(descriptor);
            removeTemporary(temporary);
            return durableError("write temporary", static_cast<std::uint64_t>(code));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const int code = errno;
        ::close(descriptor);
        removeTemporary(temporary);
        return durableError("flush temporary", static_cast<std::uint64_t>(code));
    }
    if (::close(descriptor) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("close temporary", static_cast<std::uint64_t>(code));
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int code = errno;
        removeTemporary(temporary);
        return durableError("replace target", static_cast<std::uint64_t>(code));
    }
    const std::filesystem::path parent = target.parent_path().empty()
                                             ? std::filesystem::path{"."}
                                             : target.parent_path();
    const int parentDescriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (parentDescriptor < 0) {
        return durableError("open parent directory", static_cast<std::uint64_t>(errno));
    }
    if (::fsync(parentDescriptor) != 0) {
        const int code = errno;
        ::close(parentDescriptor);
        return durableError("flush parent directory", static_cast<std::uint64_t>(code));
    }
    if (::close(parentDescriptor) != 0) {
        return durableError("close parent directory", static_cast<std::uint64_t>(errno));
    }
#endif
    return core::ok();
}

}  // namespace

json AvatarSpecCodec::toJson(const AvatarSpec& spec) const {
    const AvatarSpecDraft& values = spec.values();
    json slots = json::object();
    for (const auto& [slot, asset] : values.slots) {
        slots[std::string{toString(slot)}] = {{"assetId", asset.assetId.value()},
                                              {"version", asset.version},
                                              {"variantId", asset.variantId}};
    }

    json palette = json::object();
    for (const auto& [name, color] : values.palette) palette[name] = colorToJson(color);

    auto materials = values.materials;
    std::sort(materials.begin(), materials.end(),
              [](const MaterialOverride& left, const MaterialOverride& right) {
                  return left.channel < right.channel;
              });
    json materialJson = json::array();
    for (const auto& material : materials) {
        materialJson.push_back({{"channel", material.channel},
                                {"baseColor", colorToJson(material.baseColor)},
                                {"metallic", material.metallic}, {"roughness", material.roughness},
                                {"emission", material.emission}, {"opacity", material.opacity}});
    }

    return {{"schemaVersion", spec.schemaVersion()}, {"avatarId", values.avatarId.value()},
            {"displayName", values.displayName}, {"rigFamily", toString(values.rigFamily)},
            {"speciesFamily", values.speciesFamily}, {"styleTheme", values.styleTheme},
            {"preferredRepresentation", toString(values.preferredRepresentation)},
            {"bodyMorphs", namedScalarsToJson(values.bodyMorphs)},
            {"faceMorphs", namedScalarsToJson(values.faceMorphs)},
            {"animalMorphs", namedScalarsToJson(values.animalMorphs)}, {"slots", std::move(slots)},
            {"palette", std::move(palette)}, {"materials", std::move(materialJson)},
            {"expressions", namedScalarsToJson(values.expressions)},
            {"physics", namedScalarsToJson(values.physics)},
            {"trackingProfileId", values.trackingProfileId}};
}

Result<AvatarSpec> AvatarSpecCodec::fromJson(const json& document) const {
    if (document.is_object() && document.contains("schemaVersion")) {
        const json& version = document.at("schemaVersion");
        if (version.is_number_unsigned() &&
            version.get<std::uint64_t>() > static_cast<std::uint64_t>(AvatarSpec::kCurrentSchemaVersion)) {
            return codecError(ErrorCode::UnsupportedVersion, "avatar spec schema version is unsupported",
                              "avatar.spec.codec.version", "avatar.validation.schema-version");
        }
        if (version.is_number_integer() &&
            version.get<std::int64_t>() > static_cast<std::int64_t>(AvatarSpec::kCurrentSchemaVersion)) {
            return codecError(ErrorCode::UnsupportedVersion, "avatar spec schema version is unsupported",
                              "avatar.spec.codec.version", "avatar.validation.schema-version");
        }
    }
    if (auto valid = validate(document); !valid.hasValue()) return valid.error();
    const json& version = document.at("schemaVersion");
    const bool isCurrentVersion =
        (version.is_number_unsigned() &&
         version.get<std::uint64_t>() == static_cast<std::uint64_t>(AvatarSpec::kCurrentSchemaVersion)) ||
        (version.is_number_integer() &&
         version.get<std::int64_t>() == static_cast<std::int64_t>(AvatarSpec::kCurrentSchemaVersion));
    if (!isCurrentVersion) {
        return codecError(ErrorCode::UnsupportedVersion, "avatar spec schema version is unsupported",
                          "avatar.spec.codec.version", "avatar.validation.schema-version");
    }

    try {
        const auto avatarId = AvatarId::create(document.at("avatarId").get<std::string>());
        if (!avatarId.hasValue()) return avatarId.error();
        const auto rigFamily = rigFamilyFromString(document.at("rigFamily").get<std::string>());
        const auto representation = representationFromString(
            document.at("preferredRepresentation").get<std::string>());
        if (!rigFamily.has_value() || !representation.has_value()) {
            return codecError(ErrorCode::ParseFailure, "avatar spec contains an unsupported enum value",
                              "avatar.spec.codec.enum", "avatar.validation.schema");
        }

        AvatarSpecDraft draft{
            .avatarId = avatarId.value(), .displayName = document.at("displayName").get<std::string>(),
            .rigFamily = *rigFamily, .speciesFamily = document.at("speciesFamily").get<std::string>(),
            .styleTheme = document.at("styleTheme").get<std::string>(),
            .preferredRepresentation = *representation,
            .bodyMorphs = namedScalarsFromJson(document.at("bodyMorphs")),
            .faceMorphs = namedScalarsFromJson(document.at("faceMorphs")),
            .animalMorphs = namedScalarsFromJson(document.at("animalMorphs")),
            .palette = {}, .materials = {},
            .expressions = namedScalarsFromJson(document.at("expressions")),
            .physics = namedScalarsFromJson(document.at("physics")),
            .trackingProfileId = document.at("trackingProfileId").get<std::string>(),
        };
        for (const auto& [slotName, asset] : document.at("slots").items()) {
            const auto slot = slotFromString(slotName);
            const auto assetId = AvatarAssetId::create(asset.at("assetId").get<std::string>());
            if (!slot.has_value() || !assetId.hasValue()) {
                return codecError(ErrorCode::ParseFailure, "avatar spec contains an invalid slot asset",
                                  "avatar.spec.codec.slot", "avatar.validation.schema");
            }
            draft.slots.emplace(*slot, AssetRef{.assetId = assetId.value(),
                                                 .version = asset.at("version").get<std::string>(),
                                                 .variantId = asset.at("variantId").get<std::string>()});
        }
        for (const auto& [name, color] : document.at("palette").items()) {
            draft.palette.emplace(name, colorFromJson(color));
        }
        for (const auto& material : document.at("materials")) {
            draft.materials.push_back({.channel = material.at("channel").get<std::string>(),
                                       .baseColor = colorFromJson(material.at("baseColor")),
                                       .metallic = material.at("metallic").get<float>(),
                                       .roughness = material.at("roughness").get<float>(),
                                       .emission = material.at("emission").get<float>(),
                                       .opacity = material.at("opacity").get<float>()});
        }
        return AvatarSpec::create(std::move(draft));
    } catch (const std::exception& error) {
        return codecError(ErrorCode::ParseFailure,
                          "avatar spec could not be decoded: " + std::string{error.what()},
                          "avatar.spec.codec.decode", "avatar.validation.schema");
    }
}

Result<AvatarSpec> AvatarSpecCodec::load(const std::filesystem::path& path) const {
    if (auto safe = ensureSafePath(path); !safe.hasValue()) return safe.error();
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return codecError(ErrorCode::IoFailure, "avatar spec path could not be inspected",
                              "avatar.spec.codec.load", "avatar.validation.io");
        }
        return codecError(ErrorCode::NotFound, "avatar spec file does not exist",
                          "avatar.spec.codec.load", "avatar.validation.not-found");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return codecError(ErrorCode::IoFailure, "avatar spec file size could not be read",
                          "avatar.spec.codec.load", "avatar.validation.io");
    }
    if (size > kMaximumFileSize) {
        return codecError(ErrorCode::ParseFailure, "avatar spec file exceeds 8 MiB",
                          "avatar.spec.codec.load", "avatar.validation.size");
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return codecError(ErrorCode::IoFailure, "avatar spec file could not be opened",
                          "avatar.spec.codec.load", "avatar.validation.io");
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!stream && !stream.eof()) {
        return codecError(ErrorCode::IoFailure, "avatar spec file could not be read",
                          "avatar.spec.codec.load", "avatar.validation.io");
    }
    try {
        return fromJson(json::parse(contents));
    } catch (const std::exception& error) {
        return codecError(ErrorCode::ParseFailure,
                          "avatar spec JSON could not be parsed: " + std::string{error.what()},
                          "avatar.spec.codec.parse", "avatar.validation.json");
    }
}

Result<void> AvatarSpecCodec::save(const std::filesystem::path& path, const AvatarSpec& spec) const {
    if (auto safe = ensureSafePath(path); !safe.hasValue()) return safe.error();
    try {
        return writeAtomically(path, toJson(spec).dump(2));
    } catch (const std::exception& error) {
        return codecError(ErrorCode::ParseFailure,
                          "avatar spec JSON could not be serialized: " + std::string{error.what()},
                          "avatar.spec.codec.serialize", "avatar.validation.json");
    }
}

}  // namespace creator::avatar
