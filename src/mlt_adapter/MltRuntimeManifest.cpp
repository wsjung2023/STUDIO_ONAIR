#include "mlt_adapter/MltRuntimeManifest.h"

#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::mlt_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidState, std::move(message)};
}

class Sha256 final {
public:
    void update(std::span<const std::uint8_t> bytes) {
        totalBytes_ += bytes.size();
        while (!bytes.empty()) {
            const std::size_t copied =
                std::min(bytes.size(), buffer_.size() - buffered_);
            std::copy_n(bytes.begin(), copied, buffer_.begin() + buffered_);
            buffered_ += copied;
            bytes = bytes.subspan(copied);
            if (buffered_ == buffer_.size()) {
                transform(buffer_);
                buffered_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const std::uint64_t totalBits = totalBytes_ * 8U;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + buffered_, buffer_.end(), 0);
            transform(buffer_);
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + buffered_, buffer_.begin() + 56, 0);
        for (int index = 0; index < 8; ++index) {
            buffer_[63 - index] = static_cast<std::uint8_t>(
                totalBits >> static_cast<unsigned>(index * 8));
        }
        transform(buffer_);

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_) output << std::setw(8) << word;
        return output.str();
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    void transform(const std::array<std::uint8_t, 64>& block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                           (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
                           (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
                           static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = std::rotr(words[index - 15], 7) ^
                            std::rotr(words[index - 15], 18) ^
                            (words[index - 15] >> 3U);
            const auto s1 = std::rotr(words[index - 2], 17) ^
                            std::rotr(words[index - 2], 19) ^
                            (words[index - 2] >> 10U);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = state_;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                              std::rotr(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary1 =
                h + sum1 + choose + kRoundConstants[index] + words[index];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                              std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t totalBytes_{0};
    std::size_t buffered_{0};
};

Result<std::string> sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return invalid("Could not read an MLT runtime artifact");
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(buffer.data()),
                static_cast<std::size_t>(count)});
        }
    }
    if (!input.eof()) return invalid("Could not hash an MLT runtime artifact");
    return hash.finish();
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

std::string keyFor(std::string path) {
#ifdef _WIN32
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return path;
}

std::string asciiLower(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return path;
}

std::string fromUtf8String(const std::u8string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char8_t value : text) result.push_back(static_cast<char>(value));
    return result;
}

bool isLowerHexSha256(const std::string& text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char value) {
               return std::isdigit(value) != 0 ||
                      (value >= static_cast<unsigned char>('a') &&
                       value <= static_cast<unsigned char>('f'));
           });
}

bool isForbiddenArtifact(const std::string& path) {
    std::string lower = asciiLower(path);
    const auto slash = lower.find_last_of('/');
    const std::string name = lower.substr(slash == std::string::npos ? 0 : slash + 1);
    return name.ends_with(".exe") || name == "melt" || name == "melt.exe" ||
           name.find("plusgpl") != std::string::npos ||
           name.find("rubberband") != std::string::npos ||
           name.find("vid.stab") != std::string::npos ||
           name.find("xine") != std::string::npos;
}

struct FileProvenance final {
    std::string_view component;
    std::string_view version;
    std::string_view sourceIdentity;
    std::string_view license;
};

constexpr std::string_view kVcpkgIdentity =
    "vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e";
constexpr std::string_view kFfmpegIdentity =
    "sha256:464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c";

std::optional<FileProvenance> expectedProvenance(const std::string& path) {
    const auto lower = asciiLower(path);
    const auto nameOffset = lower.find_last_of('/');
    const auto name = lower.substr(
        nameOffset == std::string::npos ? 0 : nameOffset + 1);
    if (name.starts_with("avcodec-") || name.starts_with("avfilter-") ||
        name.starts_with("avformat-") || name.starts_with("avutil-") ||
        name.starts_with("swresample-") || name.starts_with("swscale-")) {
        return FileProvenance{"FFmpeg", "8.1.2", kFfmpegIdentity,
                              "LGPL-2.1-or-later"};
    }
    if (name == "z.dll") {
        return FileProvenance{"zlib", "1.3.2", kVcpkgIdentity, "Zlib"};
    }
    if (name.starts_with("pthread") ||
        lower.starts_with("include/mlt-deps/")) {
        return FileProvenance{"PThreads4W", "3.0.0", kVcpkgIdentity,
                              "Apache-2.0"};
    }
    if (name == "iconv-2.dll" || name == "libiconv-2.dll") {
        return FileProvenance{"GNU libiconv", "1.19", kVcpkgIdentity,
                              "LGPL-2.1-or-later"};
    }
    if (name == "dl.dll") {
        return FileProvenance{"dlfcn-win32", "1.4.2", kVcpkgIdentity,
                              "MIT"};
    }
    if (lower == "creator-studio-mlt-build.txt") {
        return FileProvenance{"Creator Studio", "1", "repository:R1-03",
                              "LicenseRef-Creator-Studio-Proprietary"};
    }
    if (lower.starts_with("bin/mlt") || lower.starts_with("lib/") ||
        lower.starts_with("share/") || lower.starts_with("include/mlt-7/")) {
        return FileProvenance{"MLT Framework", CS_MLT_EXPECTED_VERSION,
                              CS_MLT_EXPECTED_COMMIT, "LGPL-2.1-or-later"};
    }
    return std::nullopt;
}

nlohmann::json expectedDependencies() {
    return nlohmann::json::array({
        {{"component", "FFmpeg"},
         {"version", "8.1.2"},
         {"source_identity", kFfmpegIdentity},
         {"license", "LGPL-2.1-or-later"}},
        {{"component", "zlib"},
         {"version", "1.3.2"},
         {"source_identity", kVcpkgIdentity},
         {"license", "Zlib"}},
        {{"component", "PThreads4W"},
         {"version", "3.0.0"},
         {"source_identity", kVcpkgIdentity},
         {"license", "Apache-2.0"}},
        {{"component", "GNU libiconv"},
         {"version", "1.19"},
         {"source_identity", kVcpkgIdentity},
         {"license", "LGPL-2.1-or-later"}},
        {{"component", "dlfcn-win32"},
         {"version", "1.4.2"},
         {"source_identity", kVcpkgIdentity},
         {"license", "MIT"}},
    });
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const unsigned char value : text) {
        encoded.push_back(static_cast<char8_t>(value));
    }
    return std::filesystem::path{encoded};
}

Result<void> validateRelativePath(const std::string& text) {
    if (text.empty() || text.find('\\') != std::string::npos) {
        return invalid("MLT manifest contains an invalid path");
    }
    const std::filesystem::path path = pathFromUtf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        fromUtf8String(path.lexically_normal().generic_u8string()) != text) {
        return invalid("MLT manifest contains a path outside its runtime root");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return invalid("MLT manifest contains a path outside its runtime root");
        }
    }
    return core::ok();
}

}  // namespace

Result<void> verifyMltRuntimeManifest(
    const std::filesystem::path& runtimeRoot) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    const bool rootIsDirectory =
        !error && std::filesystem::is_directory(root, error);
    if (error || !rootIsDirectory || isReparsePoint(root)) {
        return invalid("MLT runtime root is missing or redirected");
    }
    const auto manifestPath = root / "mlt-runtime-manifest.json";
    const bool manifestIsFile =
        std::filesystem::is_regular_file(manifestPath, error);
    if (error || !manifestIsFile ||
        isReparsePoint(manifestPath)) {
        return AppError{ErrorCode::NotFound, "MLT runtime manifest is missing"};
    }

    nlohmann::json manifest;
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        manifest = nlohmann::json::parse(input);
        if (manifest.at("abi").get<int>() != 1 ||
            manifest.at("component").get<std::string>() != "MLT Framework" ||
            manifest.at("version").get<std::string>() != CS_MLT_EXPECTED_VERSION ||
            manifest.at("source_commit").get<std::string>() !=
                CS_MLT_EXPECTED_COMMIT ||
            manifest.at("linking").get<std::string>() != "dynamic" ||
            manifest.at("allowed_modules") !=
                nlohmann::json::array({"core", "avformat"}) ||
            manifest.at("dependencies") != expectedDependencies() ||
            !manifest.at("files").is_array()) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "MLT runtime identity is not approved"};
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "MLT runtime manifest is invalid"};
    }

    std::unordered_map<std::string, std::string> expected;
    const std::unordered_set<std::string> allowedRoles{
        "runtime-library", "runtime-module", "runtime-data", "development",
        "evidence"};
    try {
        for (const auto& entry : manifest.at("files")) {
            const auto path = entry.at("path").get<std::string>();
            const auto hash = entry.at("sha256").get<std::string>();
            const auto role = entry.at("role").get<std::string>();
            const auto component = entry.at("component").get<std::string>();
            const auto version = entry.at("version").get<std::string>();
            const auto sourceIdentity =
                entry.at("source_identity").get<std::string>();
            const auto license = entry.at("license").get<std::string>();
            if (auto valid = validateRelativePath(path); !valid.hasValue()) {
                return valid;
            }
            if (!isLowerHexSha256(hash) || !allowedRoles.contains(role)) {
                return invalid("MLT manifest contains invalid file metadata");
            }
            const auto provenance = expectedProvenance(path);
            if (!provenance || component != provenance->component ||
                version != provenance->version ||
                sourceIdentity != provenance->sourceIdentity ||
                license != provenance->license) {
                return invalid(
                    "MLT manifest contains unapproved file provenance");
            }
            if (isForbiddenArtifact(path)) {
                return invalid("MLT manifest contains a forbidden artifact");
            }
            if (!expected.emplace(keyFor(path), hash).second) {
                return invalid("MLT manifest contains a duplicate path");
            }
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "MLT runtime manifest file list is invalid"};
    }

    for (const auto* required : {"bin/mlt-7.dll", "bin/mlt++-7.dll",
                                 "bin/z.dll",
                                 "lib/mlt-7/mltcore.dll",
                                 "lib/mlt-7/mltavformat.dll"}) {
        if (!expected.contains(keyFor(required))) {
            return invalid("MLT runtime is missing a required approved component");
        }
    }

    std::unordered_map<std::string, std::filesystem::path> actual;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto path = iterator->path();
        if (isReparsePoint(path)) {
            return invalid("MLT runtime contains a redirected artifact");
        }
        if (iterator->is_regular_file(error) && !error && path != manifestPath) {
            const auto relative = fromUtf8String(
                path.lexically_relative(root).generic_u8string());
            if (auto valid = validateRelativePath(relative); !valid.hasValue()) {
                return valid;
            }
            if (isForbiddenArtifact(relative)) {
                return invalid("MLT runtime contains a forbidden artifact");
            }
            if (!actual.emplace(keyFor(relative), path).second) {
                return invalid("MLT runtime contains duplicate artifacts");
            }
        }
        iterator.increment(error);
    }
    if (error) return invalid("Could not enumerate the MLT runtime");
    if (actual.size() != expected.size()) {
        return invalid("MLT runtime file set does not match its manifest");
    }
    for (const auto& [relative, expectedHash] : expected) {
        const auto found = actual.find(relative);
        if (found == actual.end()) {
            return invalid("MLT runtime artifact is missing");
        }
        auto actualHash = sha256File(found->second);
        if (!actualHash.hasValue()) return actualHash.error();
        if (actualHash.value() != expectedHash) {
            return invalid("MLT runtime artifact hash does not match its manifest");
        }
    }
    return core::ok();
}

}  // namespace creator::mlt_adapter
