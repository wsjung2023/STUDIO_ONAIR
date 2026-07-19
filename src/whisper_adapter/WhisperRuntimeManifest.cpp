#include "whisper_adapter/WhisperRuntimeManifest.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::whisper_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidState, std::move(message)};
}

bool isReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool isLowerHexSha256(const std::string& text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char value) {
               return std::isdigit(value) != 0 ||
                      (value >= static_cast<unsigned char>('a') &&
                       value <= static_cast<unsigned char>('f'));
           });
}

std::string fromUtf8String(const std::u8string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char8_t value : text) result.push_back(static_cast<char>(value));
    return result;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const unsigned char value : text) {
        encoded.push_back(static_cast<char8_t>(value));
    }
    return std::filesystem::path{encoded};
}

// A relative, root-contained, forward-slashed path with no ".." components.
Result<void> validateRelativePath(const std::string& text) {
    if (text.empty() || text.find('\\') != std::string::npos) {
        return invalid("whisper manifest contains an invalid path");
    }
    const std::filesystem::path path = pathFromUtf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        fromUtf8String(path.lexically_normal().generic_u8string()) != text) {
        return invalid("whisper manifest contains a path outside its runtime root");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return invalid("whisper manifest contains a path outside its runtime root");
        }
    }
    return core::ok();
}

bool isForbiddenArtifact(const std::string& path) {
    const std::string lower = asciiLower(path);
    const auto slash = lower.find_last_of('/');
    const std::string name = lower.substr(slash == std::string::npos ? 0 : slash + 1);
    // whisper.cpp ships no executables into the audited prefix, and nothing GPL
    // is ever part of it. An .exe or a licence-tainted name is a tampered tree.
    return name.ends_with(".exe");
}

}  // namespace

Result<WhisperRuntimeInfo> verifyWhisperRuntimeManifest(
    const std::filesystem::path& runtimeRoot) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    const bool rootIsDirectory =
        !error && std::filesystem::is_directory(root, error);
    if (error || !rootIsDirectory || isReparsePoint(root)) {
        return invalid("whisper runtime root is missing or redirected");
    }
    const auto manifestPath = root / "whisper-runtime-manifest.json";
    const bool manifestIsFile =
        std::filesystem::is_regular_file(manifestPath, error);
    if (error || !manifestIsFile || isReparsePoint(manifestPath)) {
        return AppError{ErrorCode::NotFound, "whisper runtime manifest is missing"};
    }

    std::string modelRelative;
    std::string modelSha;
    try {
        nlohmann::json manifest;
        std::ifstream input(manifestPath, std::ios::binary);
        manifest = nlohmann::json::parse(input);

        if (manifest.at("abi").get<int>() != 1 ||
            manifest.at("component").get<std::string>() != "whisper.cpp" ||
            manifest.at("version").get<std::string>() != CS_WHISPER_EXPECTED_VERSION ||
            manifest.at("source_commit").get<std::string>() !=
                CS_WHISPER_EXPECTED_COMMIT ||
            manifest.at("linking").get<std::string>() != "dynamic" ||
            !manifest.at("files").is_array()) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "whisper runtime identity is not approved"};
        }

        const auto& model = manifest.at("model");
        modelRelative = model.at("path").get<std::string>();
        modelSha = asciiLower(model.at("sha256").get<std::string>());
        if (model.at("name").get<std::string>() != CS_WHISPER_EXPECTED_MODEL ||
            model.at("license").get<std::string>() != "MIT" ||
            modelSha != CS_WHISPER_EXPECTED_MODEL_SHA256) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "whisper runtime model identity is not approved"};
        }
        if (auto valid = validateRelativePath(modelRelative); !valid.hasValue()) {
            return valid.error();
        }

        const std::unordered_set<std::string> allowedRoles{
            "runtime-library", "model", "evidence", "development"};
        bool sawModelEntry = false;
        for (const auto& entry : manifest.at("files")) {
            const auto path = entry.at("path").get<std::string>();
            const auto hash = asciiLower(entry.at("sha256").get<std::string>());
            const auto role = entry.at("role").get<std::string>();
            const auto license = entry.at("license").get<std::string>();
            if (auto valid = validateRelativePath(path); !valid.hasValue()) {
                return valid.error();
            }
            if (!isLowerHexSha256(hash) || !allowedRoles.contains(role)) {
                return invalid("whisper manifest contains invalid file metadata");
            }
            if (isForbiddenArtifact(path)) {
                return invalid("whisper manifest contains a forbidden artifact");
            }
            // No GPL, no non-free: the whole audited prefix is MIT (whisper.cpp,
            // ggml, and the OpenAI Whisper weights) plus the proprietary
            // evidence stamp. Anything else is a tampered manifest.
            if (license.find("GPL") != std::string::npos) {
                return invalid("whisper manifest contains a GPL-licensed artifact");
            }
            if (license != "MIT" &&
                license != "LicenseRef-Creator-Studio-Proprietary") {
                return invalid("whisper manifest contains unapproved provenance");
            }

            const auto absolute = root / pathFromUtf8(path);
            if (isReparsePoint(absolute)) {
                return invalid("whisper runtime contains a redirected artifact");
            }
            auto actualHash = core::sha256File(absolute);
            if (!actualHash.hasValue()) return actualHash.error();
            if (asciiLower(actualHash.value()) != hash) {
                return invalid("whisper runtime artifact hash does not match its manifest");
            }
            if (path == modelRelative) {
                sawModelEntry = true;
                if (hash != modelSha) {
                    return invalid("whisper model hash disagrees with its manifest entry");
                }
            }
        }
        if (!sawModelEntry) {
            return invalid("whisper runtime manifest omits the pinned model");
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure, "whisper runtime manifest is invalid"};
    }

    WhisperRuntimeInfo info;
    info.modelPath = root / pathFromUtf8(modelRelative);
    info.modelSha256 = modelSha;
    return info;
}

}  // namespace creator::whisper_adapter
