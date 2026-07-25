#include "avatar/inochi2d/Inochi2dBinaryInspector.h"

#include "core/AppError.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace creator::avatar::inochi2d {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::uint64_t kMaximumBinaryBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumTableEntries = 65'536U;
constexpr std::size_t kMaximumNameBytes = 1'024U;

AppError malformed(std::string message) {
    return AppError{ErrorCode::UnsupportedVersion, std::move(message)};
}

class Bytes final {
public:
    explicit Bytes(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    [[nodiscard]] bool has(std::size_t offset,
                           std::size_t length) const noexcept {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }

    [[nodiscard]] std::optional<std::uint16_t> u16(
        std::size_t offset) const noexcept {
        if (!has(offset, 2U)) return std::nullopt;
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[offset]) |
            (static_cast<std::uint16_t>(bytes_[offset + 1U]) << 8U));
    }

    [[nodiscard]] std::optional<std::uint32_t> u32(
        std::size_t offset) const noexcept {
        if (!has(offset, 4U)) return std::nullopt;
        return static_cast<std::uint32_t>(bytes_[offset]) |
               (static_cast<std::uint32_t>(bytes_[offset + 1U]) << 8U) |
               (static_cast<std::uint32_t>(bytes_[offset + 2U]) << 16U) |
               (static_cast<std::uint32_t>(bytes_[offset + 3U]) << 24U);
    }

    [[nodiscard]] std::optional<std::string> asciiZ(
        std::size_t offset) const {
        if (!has(offset, 1U)) return std::nullopt;
        std::string value;
        value.reserve(32U);
        for (std::size_t index = offset;
             index < bytes_.size() && value.size() <= kMaximumNameBytes;
             ++index) {
            const auto byte = bytes_[index];
            if (byte == 0U) return value.empty() ? std::nullopt
                                                 : std::optional{value};
            if (byte < 0x20U || byte > 0x7eU) return std::nullopt;
            value.push_back(static_cast<char>(byte));
        }
        return std::nullopt;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

struct Section final {
    std::uint32_t virtualAddress{};
    std::uint32_t virtualSize{};
    std::uint32_t rawOffset{};
    std::uint32_t rawSize{};
};

class PeImage final {
public:
    explicit PeImage(Bytes bytes) : bytes_(std::move(bytes)) {}

    Result<Inochi2dBinaryInfo> inspect() {
        if (bytes_.size() < 64U || bytes_.u16(0U) != 0x5a4dU) {
            return malformed("Inochi2D runtime is not a PE image");
        }
        const auto peOffsetValue = bytes_.u32(0x3cU);
        if (!peOffsetValue) {
            return malformed("Inochi2D PE header is truncated");
        }
        const auto peOffset = static_cast<std::size_t>(*peOffsetValue);
        if (!bytes_.has(peOffset, 24U) ||
            bytes_.u32(peOffset) != 0x00004550U) {
            return malformed("Inochi2D PE signature is invalid");
        }
        const auto coff = peOffset + 4U;
        const auto machine = bytes_.u16(coff);
        const auto sectionCount = bytes_.u16(coff + 2U);
        const auto optionalSize = bytes_.u16(coff + 16U);
        const auto characteristics = bytes_.u16(coff + 18U);
        if (!machine || *machine != 0x8664U || !sectionCount ||
            *sectionCount == 0U || *sectionCount > 96U || !optionalSize ||
            !characteristics || (*characteristics & 0x2002U) != 0x2002U) {
            return malformed("Inochi2D image is not an x64 DLL");
        }
        const auto optional = coff + 20U;
        if (*optionalSize < 128U || !bytes_.has(optional, *optionalSize) ||
            bytes_.u16(optional) != 0x020bU) {
            return malformed("Inochi2D image is not PE32+");
        }
        const auto directoryCount = bytes_.u32(optional + 108U);
        const auto headersSize = bytes_.u32(optional + 60U);
        if (!directoryCount || *directoryCount < 2U || !headersSize) {
            return malformed("Inochi2D PE data directories are missing");
        }
        headersSize_ = *headersSize;
        const auto sectionTable = optional + *optionalSize;
        const auto sectionBytes =
            static_cast<std::size_t>(*sectionCount) * 40U;
        if (!bytes_.has(sectionTable, sectionBytes)) {
            return malformed("Inochi2D PE section table is truncated");
        }
        sections_.reserve(*sectionCount);
        for (std::size_t index = 0; index < *sectionCount; ++index) {
            const auto offset = sectionTable + index * 40U;
            const auto virtualSize = bytes_.u32(offset + 8U);
            const auto virtualAddress = bytes_.u32(offset + 12U);
            const auto rawSize = bytes_.u32(offset + 16U);
            const auto rawOffset = bytes_.u32(offset + 20U);
            if (!virtualSize || !virtualAddress || !rawSize || !rawOffset ||
                !bytes_.has(*rawOffset, *rawSize)) {
                return malformed("Inochi2D PE section is invalid");
            }
            sections_.push_back(
                {*virtualAddress, *virtualSize, *rawOffset, *rawSize});
        }

        const auto exportRva = bytes_.u32(optional + 112U);
        const auto exportSize = bytes_.u32(optional + 116U);
        const auto importRva = bytes_.u32(optional + 120U);
        const auto importSize = bytes_.u32(optional + 124U);
        if (!exportRva || !exportSize || *exportRva == 0U ||
            *exportSize < 40U || !importRva || !importSize) {
            return malformed("Inochi2D PE export directory is missing");
        }

        Inochi2dBinaryInfo info;
        auto exports = readExports(*exportRva);
        if (!exports.hasValue()) return exports.error();
        info.exports = std::move(exports).value();
        auto imports = readImports(*importRva, *importSize);
        if (!imports.hasValue()) return imports.error();
        info.imports = std::move(imports).value();
        return info;
    }

private:
    [[nodiscard]] std::optional<std::size_t> fileOffset(
        std::uint32_t rva, std::size_t length) const noexcept {
        if (rva < headersSize_ && bytes_.has(rva, length)) {
            return static_cast<std::size_t>(rva);
        }
        for (const auto& section : sections_) {
            const auto mappedSize =
                std::max(section.virtualSize, section.rawSize);
            if (rva < section.virtualAddress) continue;
            const auto delta = rva - section.virtualAddress;
            if (delta > mappedSize || length > mappedSize - delta ||
                delta > section.rawSize || length > section.rawSize - delta) {
                continue;
            }
            const auto offset =
                static_cast<std::uint64_t>(section.rawOffset) + delta;
            if (offset <= std::numeric_limits<std::size_t>::max() &&
                bytes_.has(static_cast<std::size_t>(offset), length)) {
                return static_cast<std::size_t>(offset);
            }
        }
        return std::nullopt;
    }

    Result<std::vector<std::string>> readExports(std::uint32_t rva) const {
        const auto directory = fileOffset(rva, 40U);
        if (!directory) {
            return malformed("Inochi2D PE export directory is invalid");
        }
        const auto nameCount = bytes_.u32(*directory + 24U);
        const auto namesRva = bytes_.u32(*directory + 32U);
        if (!nameCount || !namesRva || *nameCount == 0U ||
            *nameCount > kMaximumTableEntries) {
            return malformed("Inochi2D PE export name table is invalid");
        }
        const auto tableBytes = static_cast<std::size_t>(*nameCount) * 4U;
        const auto names = fileOffset(*namesRva, tableBytes);
        if (!names) {
            return malformed("Inochi2D PE export name table is truncated");
        }
        std::vector<std::string> result;
        result.reserve(*nameCount);
        for (std::size_t index = 0; index < *nameCount; ++index) {
            const auto nameRva = bytes_.u32(*names + index * 4U);
            if (!nameRva) {
                return malformed("Inochi2D PE export name is invalid");
            }
            const auto nameOffset = fileOffset(*nameRva, 1U);
            if (!nameOffset) {
                return malformed("Inochi2D PE export name escapes the image");
            }
            auto name = bytes_.asciiZ(*nameOffset);
            if (!name) {
                return malformed("Inochi2D PE export name is malformed");
            }
            result.push_back(std::move(*name));
        }
        std::sort(result.begin(), result.end());
        if (std::adjacent_find(result.begin(), result.end()) != result.end()) {
            return malformed("Inochi2D PE contains duplicate exports");
        }
        return result;
    }

    Result<std::vector<std::string>> readImports(
        std::uint32_t rva, std::uint32_t directorySize) const {
        if (rva == 0U && directorySize == 0U) {
            return std::vector<std::string>{};
        }
        if (rva == 0U || directorySize < 20U) {
            return malformed("Inochi2D PE import directory is invalid");
        }
        std::vector<std::string> result;
        bool terminated = false;
        const auto maximumDescriptors = std::min<std::size_t>(
            directorySize / 20U, kMaximumTableEntries);
        for (std::size_t index = 0; index < maximumDescriptors; ++index) {
            const auto descriptor = fileOffset(
                rva + static_cast<std::uint32_t>(index * 20U), 20U);
            if (!descriptor) {
                return malformed("Inochi2D PE import table is truncated");
            }
            bool allZero = true;
            for (std::size_t field = 0; field < 5U; ++field) {
                const auto value = bytes_.u32(*descriptor + field * 4U);
                if (!value) {
                    return malformed("Inochi2D PE import table is truncated");
                }
                allZero = allZero && *value == 0U;
            }
            if (allZero) {
                terminated = true;
                break;
            }
            const auto nameRva = bytes_.u32(*descriptor + 12U);
            if (!nameRva || *nameRva == 0U) {
                return malformed("Inochi2D PE import name is invalid");
            }
            const auto nameOffset = fileOffset(*nameRva, 1U);
            if (!nameOffset) {
                return malformed("Inochi2D PE import name escapes the image");
            }
            auto name = bytes_.asciiZ(*nameOffset);
            if (!name) {
                return malformed("Inochi2D PE import name is malformed");
            }
            std::transform(name->begin(), name->end(), name->begin(),
                           [](unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            result.push_back(std::move(*name));
        }
        if (!terminated) {
            return malformed("Inochi2D PE import table is unterminated");
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    Bytes bytes_;
    std::uint32_t headersSize_{};
    std::vector<Section> sections_;
};

Result<Bytes> readBinary(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > kMaximumBinaryBytes ||
        size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
        return AppError{ErrorCode::IoFailure,
                        "Could not read the Inochi2D runtime binary"};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input ||
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        return AppError{ErrorCode::IoFailure,
                        "Could not read the Inochi2D runtime binary"};
    }
    return Bytes{std::move(bytes)};
}

}  // namespace

Result<Inochi2dBinaryInfo> inspectWindowsX64Dll(
    const std::filesystem::path& path) {
    auto bytes = readBinary(path);
    if (!bytes.hasValue()) return bytes.error();
    return PeImage{std::move(bytes).value()}.inspect();
}

}  // namespace creator::avatar::inochi2d
