#pragma once

#include "avatar/vrm/GlbContainer.h"
#include "avatar/vrm/GltfMath.h"
#include "core/Result.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace creator::avatar::vrm {

/// One mesh primitive: the per-vertex arrays a renderer needs, already decoded
/// out of the glTF accessors into plain float data. Attributes that a primitive
/// does not declare are left empty.
struct GltfPrimitive final {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<std::uint32_t> indices;
    // Skinning: up to four joint indices + weights per vertex (empty if none).
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<Vec4> weights;
    // Morph targets: each entry is a full-length array of POSITION deltas.
    std::vector<std::vector<Vec3>> morphPositions;
    int material{-1};
};

struct GltfMesh final {
    std::vector<GltfPrimitive> primitives;
    // Default morph-target weights (glTF mesh.weights), one per morph target.
    std::vector<float> weights;
};

struct GltfNode final {
    std::string name;
    Vec3 translation{0, 0, 0};
    Quat rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
    int mesh{-1};
    int skin{-1};
    std::vector<int> children;
    [[nodiscard]] Mat4 localMatrix() const { return trs(translation, rotation, scale); }
};

struct GltfSkin final {
    std::vector<int> joints;
    std::vector<Mat4> inverseBind;  // one per joint (identity if not supplied)
    int skeleton{-1};
};

struct GltfMaterial final {
    Vec4 baseColorFactor{1, 1, 1, 1};
    int baseColorTexture{-1};
    bool doubleSided{false};
    float alphaCutoff{0.5F};
    // "OPAQUE", "MASK", or "BLEND"
    std::string alphaMode{"OPAQUE"};
};

struct GltfImage final {
    // Raw encoded bytes (PNG/JPEG) sliced from the BIN buffer, plus mime type.
    std::vector<std::byte> bytes;
    std::string mimeType;
};

/// A decoded glTF/VRM document: geometry, node hierarchy, skins, materials and
/// encoded texture bytes, ready for a renderer. Image *pixels* are not decoded
/// here (that needs an image codec); `images` holds the encoded bytes.
struct GltfDocument final {
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
    std::vector<GltfSkin> skins;
    std::vector<GltfMaterial> materials;
    std::vector<GltfImage> images;
    // texture -> image index (samplers are ignored for now).
    std::vector<int> textureImage;
    std::vector<int> sceneRoots;

    /// Parses a glb container (JSON + BIN) into a document. Validates accessor
    /// bounds against the BIN buffer, so a malformed file is rejected rather than
    /// over-read. Returns ParseFailure/UnsupportedVersion on malformed input.
    [[nodiscard]] static core::Result<GltfDocument> parse(const GlbContainer& glb);
};

}  // namespace creator::avatar::vrm
