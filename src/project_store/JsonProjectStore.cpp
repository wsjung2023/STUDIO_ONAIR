#include "project_store/JsonProjectStore.h"

#include "core/Utc.h"
#include "core/Uuid.h"
#include "domain/ProjectManifest.h"

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace creator::project_store {
namespace {

namespace fs = std::filesystem;

using core::AppError;
using core::ErrorCode;
using core::Result;
using core::Utc;
using domain::CanvasSettings;
using domain::ProjectDirectories;
using domain::ProjectId;
using domain::ProjectManifest;

/// Writes through a .part file and an atomic rename (CLAUDE.md 4).
///
/// The manifest is the file a crash is most likely to catch mid-write, and a
/// half-written manifest.json is an unopenable project. Renaming is atomic on
/// both NTFS and APFS, so a reader sees either the old file or the new one.
///
/// flush() pushes to the OS, not to the platter: a power cut can still lose the
/// write. Real durability needs fsync/FlushFileBuffers, which is platform code
/// this task does not open. Atomicity is what protects us here, and it holds.
Result<void> writeFileAtomically(const fs::path& target, const std::string& contents) {
    const fs::path temporary = target.string() + ".part";

    {
        std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
        if (!out.is_open()) {
            return AppError{ErrorCode::IoFailure,
                            "cannot open '" + temporary.string() + "' for writing"};
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return AppError{ErrorCode::IoFailure, "failed writing '" + temporary.string() + "'"};
        }
    }

    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return AppError{ErrorCode::IoFailure,
                        "cannot move '" + temporary.string() + "' into place: " + ec.message()};
    }
    return core::ok();
}

Result<std::string> readFile(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return AppError{ErrorCode::NotFound, "no manifest at '" + path.string() + "'"};
    }
    std::ifstream in{path, std::ios::binary};
    if (!in.is_open()) {
        return AppError{ErrorCode::IoFailure, "cannot open '" + path.string() + "' for reading"};
    }
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

nlohmann::json toJson(const ProjectManifest& manifest) {
    // Key names and nesting mirror schemas/project.schema.json exactly. The
    // schema sets additionalProperties false, so an extra key invented here
    // would make every manifest we write fail validation in R0-02.
    return nlohmann::json{
        {"schemaVersion", manifest.schemaVersion},
        {"projectId", manifest.projectId.value()},
        {"name", manifest.name},
        {"createdAt", manifest.createdAt.toRfc3339()},
        {"updatedAt", manifest.updatedAt.toRfc3339()},
        {"canvas",
         {{"width", manifest.canvas.width},
          {"height", manifest.canvas.height},
          {"frameRateNumerator", manifest.canvas.frameRateNumerator},
          {"frameRateDenominator", manifest.canvas.frameRateDenominator},
          {"colorSpace", domain::toString(manifest.canvas.colorSpace)}}},
        {"database", manifest.database},
        {"directories",
         {{"media", manifest.directories.media},
          {"audio", manifest.directories.audio},
          {"telemetry", manifest.directories.telemetry},
          {"proxies", manifest.directories.proxies},
          {"thumbnails", manifest.directories.thumbnails},
          {"autosave", manifest.directories.autosave},
          {"renders", manifest.directories.renders},
          {"logs", manifest.directories.logs}}},
        {"requiredFeatures", manifest.requiredFeatures},
    };
}

/// Reads a required field, reporting a missing key and a wrong type the same
/// way: both mean the file does not describe a project we can open.
template <typename T>
Result<T> requireField(const nlohmann::json& object, std::string_view key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return AppError{ErrorCode::ParseFailure, "manifest is missing '" + std::string{key} + "'"};
    }
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "manifest field '" + std::string{key} + "' has the wrong type"};
    }
}

Result<CanvasSettings> parseCanvas(const nlohmann::json& json) {
    auto width = requireField<std::int32_t>(json, "width");
    if (!width.hasValue()) return width.error();
    auto height = requireField<std::int32_t>(json, "height");
    if (!height.hasValue()) return height.error();
    auto numerator = requireField<std::int64_t>(json, "frameRateNumerator");
    if (!numerator.hasValue()) return numerator.error();
    auto denominator = requireField<std::int64_t>(json, "frameRateDenominator");
    if (!denominator.hasValue()) return denominator.error();
    auto colorSpaceText = requireField<std::string>(json, "colorSpace");
    if (!colorSpaceText.hasValue()) return colorSpaceText.error();

    auto colorSpace = domain::parseColorSpace(colorSpaceText.value());
    if (!colorSpace.hasValue()) return colorSpace.error();

    return CanvasSettings{
        .width = width.value(),
        .height = height.value(),
        .frameRateNumerator = numerator.value(),
        .frameRateDenominator = denominator.value(),
        .colorSpace = colorSpace.value(),
    };
}

Result<ProjectDirectories> parseDirectories(const nlohmann::json& json) {
    ProjectDirectories directories;
    const std::array<std::pair<const char*, std::string*>, 8> fields{{
        {"media", &directories.media},
        {"audio", &directories.audio},
        {"telemetry", &directories.telemetry},
        {"proxies", &directories.proxies},
        {"thumbnails", &directories.thumbnails},
        {"autosave", &directories.autosave},
        {"renders", &directories.renders},
        {"logs", &directories.logs},
    }};
    for (const auto& [key, target] : fields) {
        auto value = requireField<std::string>(json, key);
        if (!value.hasValue()) return value.error();
        *target = std::move(value).value();
    }
    return directories;
}

Result<ProjectManifest> fromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return AppError{ErrorCode::ParseFailure, "manifest root must be a JSON object"};
    }

    auto schemaVersion = requireField<std::int32_t>(json, "schemaVersion");
    if (!schemaVersion.hasValue()) return schemaVersion.error();
    // Checked before anything else: a newer manifest may have fields we would
    // otherwise misparse, and the honest answer is "this build is too old".
    if (schemaVersion.value() > ProjectManifest::kCurrentSchemaVersion) {
        return AppError{ErrorCode::UnsupportedVersion,
                        "project was written by a newer version of Creator Studio"};
    }

    auto projectIdText = requireField<std::string>(json, "projectId");
    if (!projectIdText.hasValue()) return projectIdText.error();
    auto projectId = ProjectId::create(projectIdText.value());
    if (!projectId.hasValue()) return projectId.error();

    auto name = requireField<std::string>(json, "name");
    if (!name.hasValue()) return name.error();

    auto createdAtText = requireField<std::string>(json, "createdAt");
    if (!createdAtText.hasValue()) return createdAtText.error();
    auto createdAt = Utc::parseRfc3339(createdAtText.value());
    if (!createdAt.hasValue()) return createdAt.error();

    auto updatedAtText = requireField<std::string>(json, "updatedAt");
    if (!updatedAtText.hasValue()) return updatedAtText.error();
    auto updatedAt = Utc::parseRfc3339(updatedAtText.value());
    if (!updatedAt.hasValue()) return updatedAt.error();

    const auto canvasJson = json.find("canvas");
    if (canvasJson == json.end()) {
        return AppError{ErrorCode::ParseFailure, "manifest is missing 'canvas'"};
    }
    auto canvas = parseCanvas(*canvasJson);
    if (!canvas.hasValue()) return canvas.error();

    auto database = requireField<std::string>(json, "database");
    if (!database.hasValue()) return database.error();

    const auto directoriesJson = json.find("directories");
    if (directoriesJson == json.end()) {
        return AppError{ErrorCode::ParseFailure, "manifest is missing 'directories'"};
    }
    auto directories = parseDirectories(*directoriesJson);
    if (!directories.hasValue()) return directories.error();

    // requiredFeatures has a schema default of [], so absence is legal.
    std::vector<std::string> requiredFeatures;
    if (const auto it = json.find("requiredFeatures"); it != json.end()) {
        try {
            requiredFeatures = it->get<std::vector<std::string>>();
        } catch (const nlohmann::json::exception&) {
            return AppError{ErrorCode::ParseFailure,
                            "manifest field 'requiredFeatures' has the wrong type"};
        }
    }

    ProjectManifest manifest{
        .schemaVersion = schemaVersion.value(),
        .projectId = std::move(projectId).value(),
        .name = std::move(name).value(),
        .createdAt = createdAt.value(),
        .updatedAt = updatedAt.value(),
        .canvas = canvas.value(),
        .database = std::move(database).value(),
        .directories = std::move(directories).value(),
        .requiredFeatures = std::move(requiredFeatures),
    };

    if (auto valid = domain::validate(manifest); !valid.hasValue()) {
        return valid.error();
    }
    return manifest;
}

}  // namespace

Result<ProjectManifest> JsonProjectStore::create(const fs::path& packageDirectory,
                                                 const std::string& name) {
    const fs::path manifestPath = packageDirectory / kManifestFileName;

    std::error_code ec;
    if (fs::exists(manifestPath, ec)) {
        // Never overwrite: that directory holds someone's recording.
        return AppError{ErrorCode::AlreadyExists,
                        "a project already exists at '" + packageDirectory.string() + "'"};
    }

    const Utc now = Utc::now();
    auto projectId = ProjectId::create(core::generateUuidV4());
    if (!projectId.hasValue()) return projectId.error();

    ProjectManifest manifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = std::move(projectId).value(),
        .name = name,
        .createdAt = now,
        .updatedAt = now,
        .canvas = CanvasSettings{},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = ProjectDirectories{},
        .requiredFeatures = {},
    };

    // Validate before touching the disk, so a bad name leaves no directories.
    if (auto valid = domain::validate(manifest); !valid.hasValue()) {
        return valid.error();
    }

    fs::create_directories(packageDirectory, ec);
    if (ec) {
        return AppError{ErrorCode::IoFailure,
                        "cannot create '" + packageDirectory.string() + "': " + ec.message()};
    }
    for (const std::string* directory :
         {&manifest.directories.media, &manifest.directories.audio,
          &manifest.directories.telemetry, &manifest.directories.proxies,
          &manifest.directories.thumbnails, &manifest.directories.autosave,
          &manifest.directories.renders, &manifest.directories.logs}) {
        fs::create_directories(packageDirectory / *directory, ec);
        if (ec) {
            return AppError{ErrorCode::IoFailure,
                            "cannot create '" + *directory + "': " + ec.message()};
        }
    }

    if (auto written = writeFileAtomically(manifestPath, toJson(manifest).dump(2));
        !written.hasValue()) {
        return written.error();
    }
    return manifest;
}

Result<ProjectManifest> JsonProjectStore::load(const fs::path& packageDirectory) {
    auto text = readFile(packageDirectory / kManifestFileName);
    if (!text.hasValue()) return text.error();

    // nlohmann throws on malformed input; the boundary stops here so a corrupt
    // file surfaces as a product error rather than an exception crossing a
    // module boundary (CLAUDE.md 4).
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text.value());
    } catch (const nlohmann::json::parse_error& error) {
        return AppError{ErrorCode::ParseFailure,
                        "manifest is not valid JSON: " + std::string{error.what()}};
    }
    return fromJson(json);
}

Result<void> JsonProjectStore::save(const fs::path& packageDirectory,
                                    const ProjectManifest& manifest) {
    // Validate first: a rejected manifest must not reach the disk, or a bad
    // save would destroy the good file already there.
    if (auto valid = domain::validate(manifest); !valid.hasValue()) {
        return valid.error();
    }
    return writeFileAtomically(packageDirectory / kManifestFileName, toJson(manifest).dump(2));
}

}  // namespace creator::project_store
