#include "avatar/vrm/GltfDocument.h"

#include "avatar/vrm/GlbContainer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using creator::avatar::vrm::GlbContainer;
using creator::avatar::vrm::GltfDocument;

void appendU32(std::vector<std::byte>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        o.push_back(static_cast<std::byte>((v >> (i * 8)) & 0xFFU));
}
void appendF32(std::vector<std::byte>& o, float f) {
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    appendU32(o, v);
}
void appendU16(std::vector<std::byte>& o, std::uint16_t v) {
    o.push_back(static_cast<std::byte>(v & 0xFFU));
    o.push_back(static_cast<std::byte>((v >> 8) & 0xFFU));
}

std::vector<std::byte> makeGlb(const std::string& json,
                               const std::vector<std::byte>& bin) {
    std::vector<std::byte> jsonChunk(json.size());
    std::memcpy(jsonChunk.data(), json.data(), json.size());
    while (jsonChunk.size() % 4U != 0U) jsonChunk.push_back(static_cast<std::byte>(0x20));
    std::vector<std::byte> binChunk = bin;
    while (binChunk.size() % 4U != 0U) binChunk.push_back(static_cast<std::byte>(0));

    std::vector<std::byte> body;
    appendU32(body, static_cast<std::uint32_t>(jsonChunk.size()));
    appendU32(body, 0x4E4F534AU);
    body.insert(body.end(), jsonChunk.begin(), jsonChunk.end());
    appendU32(body, static_cast<std::uint32_t>(binChunk.size()));
    appendU32(body, 0x004E4942U);
    body.insert(body.end(), binChunk.begin(), binChunk.end());

    std::vector<std::byte> glb;
    appendU32(glb, 0x46546C67U);
    appendU32(glb, 2U);
    appendU32(glb, static_cast<std::uint32_t>(12U + body.size()));
    glb.insert(glb.end(), body.begin(), body.end());
    return glb;
}

TEST(GltfDocumentTest, ExtractsTriangleGeometryAndMaterial) {
    // BIN: three VEC3 positions (36 bytes) then three USHORT indices (6 bytes).
    std::vector<std::byte> bin;
    appendF32(bin, 0);  appendF32(bin, 0);  appendF32(bin, 0);
    appendF32(bin, 1);  appendF32(bin, 0);  appendF32(bin, 0);
    appendF32(bin, 0);  appendF32(bin, 1);  appendF32(bin, 0);
    appendU16(bin, 0);  appendU16(bin, 1);  appendU16(bin, 2);

    const std::string json = R"({
      "asset":{"version":"2.0"},
      "buffers":[{"byteLength":42}],
      "bufferViews":[
        {"buffer":0,"byteOffset":0,"byteLength":36},
        {"buffer":0,"byteOffset":36,"byteLength":6}
      ],
      "accessors":[
        {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
        {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}
      ],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
      "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1.0,0.5,0.25,1.0]}}],
      "nodes":[{"mesh":0}],
      "scenes":[{"nodes":[0]}],"scene":0
    })";

    const auto glb = GlbContainer::read(makeGlb(json, bin));
    ASSERT_TRUE(glb.hasValue()) << glb.error().message();
    const auto doc = GltfDocument::parse(glb.value());
    ASSERT_TRUE(doc.hasValue()) << doc.error().message();

    ASSERT_EQ(doc.value().meshes.size(), 1U);
    ASSERT_EQ(doc.value().meshes[0].primitives.size(), 1U);
    const auto& prim = doc.value().meshes[0].primitives[0];
    ASSERT_EQ(prim.positions.size(), 3U);
    EXPECT_FLOAT_EQ(prim.positions[1].x, 1.0F);
    EXPECT_FLOAT_EQ(prim.positions[2].y, 1.0F);
    ASSERT_EQ(prim.indices.size(), 3U);
    EXPECT_EQ(prim.indices[2], 2U);
    EXPECT_EQ(prim.material, 0);

    ASSERT_EQ(doc.value().materials.size(), 1U);
    EXPECT_FLOAT_EQ(doc.value().materials[0].baseColorFactor.y, 0.5F);
    ASSERT_EQ(doc.value().nodes.size(), 1U);
    EXPECT_EQ(doc.value().nodes[0].mesh, 0);
    ASSERT_EQ(doc.value().sceneRoots.size(), 1U);
}

TEST(GltfDocumentTest, RejectsAccessorPastBuffer) {
    std::vector<std::byte> bin;
    appendF32(bin, 0);  appendF32(bin, 0);  appendF32(bin, 0);  // only one vec3
    const std::string json = R"({
      "asset":{"version":"2.0"},
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":100,"type":"VEC3"}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}]
    })";
    const auto glb = GlbContainer::read(makeGlb(json, bin));
    ASSERT_TRUE(glb.hasValue());
    const auto doc = GltfDocument::parse(glb.value());
    // The out-of-range accessor yields no positions rather than over-reading.
    ASSERT_TRUE(doc.hasValue()) << doc.error().message();
    EXPECT_TRUE(doc.value().meshes[0].primitives[0].positions.empty());
}

}  // namespace
