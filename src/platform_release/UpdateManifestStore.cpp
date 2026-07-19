#include "platform_release/UpdateManifestStore.h"

#include "core/AppError.h"
#include "core/Uuid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace creator::platform_release {
namespace {

constexpr std::uintmax_t kMaximumManifestBytes = 1024U * 1024U;

std::string encodeHex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        encoded.push_back(digits[(value >> 4U) & 0x0fU]);
        encoded.push_back(digits[value & 0x0fU]);
    }
    return encoded;
}

core::Result<std::vector<std::byte>> decodeHex(std::string_view encoded) {
    if (encoded.empty() || encoded.size() > 16'384 || encoded.size() % 2 != 0) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "update signature encoding is invalid"};
    }
    const auto nibble = [](unsigned char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    std::vector<std::byte> bytes;
    bytes.reserve(encoded.size() / 2);
    for (std::size_t index = 0; index < encoded.size(); index += 2) {
        const auto high = nibble(static_cast<unsigned char>(encoded[index]));
        const auto low = nibble(static_cast<unsigned char>(encoded[index + 1]));
        if (high < 0 || low < 0) {
            return core::AppError{core::ErrorCode::ParseFailure,
                                  "update signature encoding is invalid"};
        }
        bytes.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return bytes;
}

core::Result<void> replaceAtomically(const std::filesystem::path& temporary,
                                     const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "update manifest could not be atomically replaced"};
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "update manifest could not be atomically replaced"};
    }
#endif
    return core::ok();
}

}  // namespace

core::Result<void> UpdateManifestStore::write(
    const std::filesystem::path& path, const UpdateManifest& manifest,
    std::span<const std::byte> signature) const {
    if (path.empty() || path.filename().empty() || signature.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "update manifest path and signature are required"};
    }
    std::error_code error;
    const auto parent = path.has_parent_path() ? path.parent_path()
                                                : std::filesystem::current_path(error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "update manifest directory is unavailable"};
    }
    std::filesystem::create_directories(parent, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "update manifest directory could not be created"};
    }

    const auto temporary =
        parent / ("." + path.filename().string() + ".part-" + core::generateUuidV4());
    const nlohmann::json envelope{{"payload", manifest.toPayloadJson()},
                                  {"signature", encodeHex(signature)}};
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "update manifest temporary file could not be opened"};
        }
        output << envelope.dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::filesystem::remove(temporary, error);
            return core::AppError{core::ErrorCode::IoFailure,
                                  "update manifest temporary file could not be written"};
        }
    }
    const auto replaced = replaceAtomically(temporary, path);
    if (!replaced.hasValue()) {
        std::filesystem::remove(temporary, error);
        return replaced.error();
    }
    return core::ok();
}

core::Result<UpdateManifest> UpdateManifestStore::loadVerified(
    const std::filesystem::path& path,
    const IUpdateSignatureVerifier& verifier) const {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::AppError{core::ErrorCode::NotFound,
                              "update manifest does not exist"};
    }
    if (size == 0 || size > kMaximumManifestBytes) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "update manifest size is invalid"};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "update manifest could not be read"};
    }
    std::stringstream contents;
    contents << input.rdbuf();
    try {
        const auto envelope = nlohmann::json::parse(contents.str());
        if (!envelope.is_object() || envelope.size() != 2 ||
            !envelope.contains("payload") || !envelope.contains("signature") ||
            !envelope.at("signature").is_string()) {
            return core::AppError{core::ErrorCode::ParseFailure,
                                  "update manifest envelope is invalid"};
        }
        const auto manifest = UpdateManifest::fromPayloadJson(envelope.at("payload"));
        if (!manifest.hasValue()) return manifest.error();
        const auto signature =
            decodeHex(envelope.at("signature").get<std::string>());
        if (!signature.hasValue()) return signature.error();
        const auto verified = verifier.verify(manifest.value().canonicalPayload(),
                                              signature.value());
        if (!verified.hasValue()) return verified.error();
        return manifest.value();
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "update manifest is not valid JSON"};
    }
}

}  // namespace creator::platform_release
