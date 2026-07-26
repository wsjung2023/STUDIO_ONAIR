#include "avatar/vrm/GlbContainer.h"

#include "core/AppError.h"

#include <cstring>

namespace creator::avatar::vrm {
namespace {

using core::AppError;
using core::ErrorCode;

constexpr std::uint32_t kMagic = 0x46546C67U;      // "glTF" little-endian
constexpr std::uint32_t kJsonChunkType = 0x4E4F534AU;  // "JSON"
constexpr std::uint32_t kBinChunkType = 0x004E4942U;   // "BIN\0"
constexpr std::uint32_t kSupportedVersion = 2U;

// Hard ceilings so a corrupt/hostile length can never request a huge allocation.
constexpr std::size_t kMaxJsonBytes = 64U * 1024U * 1024U;        // 64 MiB
constexpr std::size_t kMaxBinBytes = 512U * 1024U * 1024U;        // 512 MiB

// Reads a little-endian uint32 at `offset`; caller guarantees 4 bytes remain.
std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;  // glTF is little-endian; hosts we target are too.
}

}  // namespace

core::Result<GlbContainer> GlbContainer::read(std::span<const std::byte> bytes) {
    if (bytes.size() < 12U) {
        return AppError{ErrorCode::ParseFailure,
                        "glb container is smaller than its 12-byte header"};
    }
    if (readU32(bytes, 0U) != kMagic) {
        return AppError{ErrorCode::ParseFailure,
                        "glb container has an invalid magic (not a .glb/.vrm)"};
    }
    if (readU32(bytes, 4U) != kSupportedVersion) {
        return AppError{ErrorCode::UnsupportedVersion,
                        "glb container is not glTF binary version 2"};
    }
    const std::uint32_t declaredLength = readU32(bytes, 8U);
    if (declaredLength > bytes.size()) {
        return AppError{ErrorCode::ParseFailure,
                        "glb declared length exceeds the file"};
    }
    // Use the declared length as the authority, clamped to what we actually have.
    const std::size_t total = declaredLength;

    GlbContainer out;
    bool haveJson = false;
    std::size_t offset = 12U;
    while (offset + 8U <= total) {
        const std::uint32_t chunkLength = readU32(bytes, offset);
        const std::uint32_t chunkType = readU32(bytes, offset + 4U);
        offset += 8U;
        // The chunk body must fit inside the declared container.
        if (chunkLength > total - offset) {
            return AppError{ErrorCode::ParseFailure,
                            "glb chunk length runs past the container"};
        }
        const auto* body = bytes.data() + offset;
        if (chunkType == kJsonChunkType) {
            if (haveJson) {
                return AppError{ErrorCode::ParseFailure,
                                "glb container has more than one JSON chunk"};
            }
            if (chunkLength > kMaxJsonBytes) {
                return AppError{ErrorCode::InvalidArgument,
                                "glb JSON chunk exceeds the supported size"};
            }
            out.json.assign(reinterpret_cast<const char*>(body), chunkLength);
            // The JSON chunk is padded to 4 bytes with trailing spaces (or NULs);
            // trim them so the returned document is exactly the glTF JSON.
            while (!out.json.empty() &&
                   (out.json.back() == ' ' || out.json.back() == '\0')) {
                out.json.pop_back();
            }
            haveJson = true;
        } else if (chunkType == kBinChunkType) {
            if (chunkLength > kMaxBinBytes) {
                return AppError{ErrorCode::InvalidArgument,
                                "glb BIN chunk exceeds the supported size"};
            }
            out.bin.assign(body, body + chunkLength);
        }
        // Unknown chunk types are skipped per the glTF spec.
        offset += chunkLength;
    }
    if (!haveJson) {
        return AppError{ErrorCode::ParseFailure,
                        "glb container has no JSON chunk"};
    }
    return out;
}

}  // namespace creator::avatar::vrm
