#include "platform_release/UpdateManifest.h"

#include "core/AppError.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace creator::platform_release {
namespace {

bool hasExactKeys(const nlohmann::json& object,
                  std::initializer_list<std::string_view> expected) {
    if (!object.is_object() || object.size() != expected.size()) return false;
    return std::all_of(expected.begin(), expected.end(), [&](std::string_view key) {
        return object.contains(key);
    });
}

bool isSemVer(std::string_view value) {
    static const std::regex pattern{
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$)"};
    return value.size() <= 64 && std::regex_match(value.begin(), value.end(), pattern);
}

bool isToken(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '-' ||
                      character == '_';
           });
}

bool isSha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

bool isCanonicalRelativePath(std::string_view value) {
    if (value.empty() || value.size() > 512 ||
        value.find('\\') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path{std::string{value}};
    if (path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part == "." || part == ".." || part.empty()) return false;
    }
    return path.generic_string() == value;
}

bool isHttpsUrl(std::string_view value) {
    if (value.size() < 10 || value.size() > 2048 ||
        !value.starts_with("https://") || value.find('\\') != std::string_view::npos ||
        value.find_first_of("\r\n\t ") != std::string_view::npos ||
        value.find_first_of("?#") != std::string_view::npos) {
        return false;
    }
    const auto authorityStart = std::string_view{"https://"}.size();
    const auto pathStart = value.find('/', authorityStart);
    const auto authority = value.substr(
        authorityStart, pathStart == std::string_view::npos
                            ? std::string_view::npos
                            : pathStart - authorityStart);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return false;

    std::string lowered{value};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (lowered.find("%2e") != std::string::npos) return false;

    if (pathStart != std::string_view::npos) {
        auto remaining = value.substr(pathStart + 1);
        while (!remaining.empty()) {
            const auto separator = remaining.find('/');
            const auto segment = remaining.substr(0, separator);
            if (segment == "." || segment == "..") return false;
            if (separator == std::string_view::npos) break;
            remaining.remove_prefix(separator + 1);
        }
    }
    return true;
}

}  // namespace

core::Result<UpdateManifest> UpdateManifest::create(
    std::string channel, std::string productVersion,
    std::vector<UpdateTarget> targets) {
    if ((channel != "stable" && channel != "beta") || !isSemVer(productVersion)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "update channel or product version is invalid"};
    }
    if (targets.empty() || targets.size() > 32) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "update manifest target count is invalid"};
    }

    std::unordered_set<std::string> platforms;
    for (const auto& target : targets) {
        if (!isToken(target.platform) ||
            !isCanonicalRelativePath(target.artifact) || !isHttpsUrl(target.url) ||
            !isSha256(target.sha256) || target.sizeBytes == 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "update target metadata is invalid"};
        }
        if (!platforms.insert(target.platform).second) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "update target platforms must be unique"};
        }
    }
    std::sort(targets.begin(), targets.end(),
              [](const UpdateTarget& left, const UpdateTarget& right) {
                  return left.platform < right.platform;
              });
    return UpdateManifest{std::move(channel), std::move(productVersion),
                          std::move(targets)};
}

nlohmann::json UpdateManifest::toPayloadJson() const {
    nlohmann::json targets = nlohmann::json::array();
    for (const auto& target : targets_) {
        targets.push_back({{"artifact", target.artifact},
                           {"platform", target.platform},
                           {"sha256", target.sha256},
                           {"sizeBytes", target.sizeBytes},
                           {"url", target.url}});
    }
    return {{"channel", channel_},
            {"productVersion", productVersion_},
            {"schemaVersion", 1},
            {"targets", std::move(targets)}};
}

std::string UpdateManifest::canonicalPayload() const {
    return toPayloadJson().dump();
}

core::Result<UpdateManifest> UpdateManifest::fromPayloadJson(
    const nlohmann::json& document) {
    try {
        if (!hasExactKeys(document,
                          {"channel", "productVersion", "schemaVersion", "targets"}) ||
            document.at("schemaVersion") != 1 || !document.at("targets").is_array()) {
            return core::AppError{core::ErrorCode::UnsupportedVersion,
                                  "update manifest schema is unsupported"};
        }
        std::vector<UpdateTarget> targets;
        for (const auto& target : document.at("targets")) {
            if (!hasExactKeys(target, {"artifact", "platform", "sha256",
                                       "sizeBytes", "url"})) {
                return core::AppError{core::ErrorCode::ParseFailure,
                                      "update target contains unknown or missing fields"};
            }
            targets.push_back({target.at("platform").get<std::string>(),
                               target.at("artifact").get<std::string>(),
                               target.at("url").get<std::string>(),
                               target.at("sha256").get<std::string>(),
                               target.at("sizeBytes").get<std::uint64_t>()});
        }
        return create(document.at("channel").get<std::string>(),
                      document.at("productVersion").get<std::string>(),
                      std::move(targets));
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "update manifest is not valid structured metadata"};
    }
}

}  // namespace creator::platform_release
