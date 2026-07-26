#include "avatar/vrm/GltfDocument.h"

#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <cstring>

namespace creator::avatar::vrm {
namespace {

using core::AppError;
using core::ErrorCode;
using nlohmann::json;

// glTF component types.
constexpr int kByte = 5120, kUByte = 5121, kShort = 5122, kUShort = 5123,
              kUInt = 5125, kFloat = 5126;

int componentSize(int ct) {
    switch (ct) {
        case kByte:
        case kUByte:
            return 1;
        case kShort:
        case kUShort:
            return 2;
        case kUInt:
        case kFloat:
            return 4;
        default:
            return 0;
    }
}

int typeComponents(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    if (t == "MAT4") return 16;
    return 0;
}

// Reads one numeric component and, for `normalized` integer accessors, maps it
// to the glTF normalized float range.
float readComponent(const std::byte* p, int ct, bool normalized) {
    switch (ct) {
        case kFloat: {
            float v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        case kUByte: {
            const auto v = static_cast<std::uint8_t>(*p);
            return normalized ? static_cast<float>(v) / 255.0F : static_cast<float>(v);
        }
        case kByte: {
            const auto v = static_cast<std::int8_t>(*p);
            return normalized ? std::max(static_cast<float>(v) / 127.0F, -1.0F)
                              : static_cast<float>(v);
        }
        case kUShort: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<float>(v) / 65535.0F : static_cast<float>(v);
        }
        case kShort: {
            std::int16_t v = 0;
            std::memcpy(&v, p, 2);
            return normalized ? std::max(static_cast<float>(v) / 32767.0F, -1.0F)
                              : static_cast<float>(v);
        }
        case kUInt: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v);
        }
        default:
            return 0;
    }
}

// Bounded accessor decoder over a single BIN buffer. All bounds are validated
// against the buffer size before any read.
class AccessorReader final {
public:
    AccessorReader(const json& doc, const std::vector<std::byte>& bin)
        : doc_(doc), bin_(bin) {}

    // Fills `out` (count * comps floats). Returns false on any malformed bound.
    bool read(int accessorIndex, std::vector<float>& out, int& compsOut) {
        if (!doc_.contains("accessors")) return false;
        const auto& accessors = doc_["accessors"];
        if (accessorIndex < 0 ||
            static_cast<std::size_t>(accessorIndex) >= accessors.size()) {
            return false;
        }
        const auto& a = accessors[static_cast<std::size_t>(accessorIndex)];
        const int ct = a.value("componentType", 0);
        const std::string type = a.value("type", "");
        const auto count = a.value("count", 0U);
        const bool normalized = a.value("normalized", false);
        const int comps = typeComponents(type);
        const int csz = componentSize(ct);
        if (comps == 0 || csz == 0 || count == 0U) return false;
        compsOut = comps;

        // bufferView (a sparse-only accessor is unsupported here).
        if (!a.contains("bufferView")) return false;
        const int bvIndex = a["bufferView"].get<int>();
        std::size_t bvOffset = 0, bvLength = 0, bvStride = 0;
        if (!bufferView(bvIndex, bvOffset, bvLength, bvStride)) return false;

        const std::size_t accByteOffset = a.value("byteOffset", 0U);
        const std::size_t elemSize = static_cast<std::size_t>(comps) * csz;
        const std::size_t stride = bvStride != 0 ? bvStride : elemSize;
        const std::size_t start = bvOffset + accByteOffset;
        // Last element's last byte must be within the buffer.
        const std::size_t lastElem = start + stride * (count - 1U) + elemSize;
        if (lastElem > bin_.size() || start > bin_.size()) return false;

        out.resize(static_cast<std::size_t>(count) * comps);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::byte* base = bin_.data() + start + stride * i;
            for (int c = 0; c < comps; ++c) {
                out[static_cast<std::size_t>(i) * comps + c] =
                    readComponent(base + static_cast<std::size_t>(c) * csz, ct,
                                  normalized);
            }
        }
        return true;
    }

private:
    bool bufferView(int index, std::size_t& offset, std::size_t& length,
                    std::size_t& stride) {
        if (!doc_.contains("bufferViews")) return false;
        const auto& views = doc_["bufferViews"];
        if (index < 0 || static_cast<std::size_t>(index) >= views.size()) return false;
        const auto& v = views[static_cast<std::size_t>(index)];
        offset = v.value("byteOffset", 0U);
        length = v.value("byteLength", 0U);
        stride = v.value("byteStride", 0U);
        if (offset + length > bin_.size()) return false;
        return true;
    }

    const json& doc_;
    const std::vector<std::byte>& bin_;
};

std::vector<Vec3> asVec3(const std::vector<float>& f, int comps) {
    std::vector<Vec3> out;
    if (comps < 3) return out;
    out.reserve(f.size() / comps);
    for (std::size_t i = 0; i + 2 < f.size(); i += comps)
        out.push_back({f[i], f[i + 1], f[i + 2]});
    return out;
}

}  // namespace

core::Result<GltfDocument> GltfDocument::parse(const GlbContainer& glb) {
    json doc;
    try {
        doc = json::parse(glb.json);
    } catch (const std::exception&) {
        return AppError{ErrorCode::ParseFailure, "glTF JSON is not valid JSON"};
    }
    if (!doc.is_object()) {
        return AppError{ErrorCode::ParseFailure, "glTF root is not an object"};
    }

    GltfDocument out;
    AccessorReader reader{doc, glb.bin};

    // ---- Meshes ----
    if (doc.contains("meshes")) {
        for (const auto& mesh : doc["meshes"]) {
            GltfMesh gm;
            if (mesh.contains("weights")) {
                for (const auto& w : mesh["weights"]) gm.weights.push_back(w.get<float>());
            }
            for (const auto& prim : mesh.value("primitives", json::array())) {
                GltfPrimitive gp;
                gp.material = prim.value("material", -1);
                const auto& attrs = prim.value("attributes", json::object());
                std::vector<float> f;
                int comps = 0;
                if (attrs.contains("POSITION") &&
                    reader.read(attrs["POSITION"].get<int>(), f, comps)) {
                    gp.positions = asVec3(f, comps);
                }
                if (attrs.contains("NORMAL") &&
                    reader.read(attrs["NORMAL"].get<int>(), f, comps)) {
                    gp.normals = asVec3(f, comps);
                }
                if (attrs.contains("TEXCOORD_0") &&
                    reader.read(attrs["TEXCOORD_0"].get<int>(), f, comps) && comps >= 2) {
                    for (std::size_t i = 0; i + 1 < f.size(); i += comps)
                        gp.uvs.push_back({f[i], f[i + 1]});
                }
                if (attrs.contains("JOINTS_0") &&
                    reader.read(attrs["JOINTS_0"].get<int>(), f, comps) && comps >= 4) {
                    for (std::size_t i = 0; i + 3 < f.size(); i += comps) {
                        gp.joints.push_back(
                            {static_cast<std::uint16_t>(f[i]),
                             static_cast<std::uint16_t>(f[i + 1]),
                             static_cast<std::uint16_t>(f[i + 2]),
                             static_cast<std::uint16_t>(f[i + 3])});
                    }
                }
                if (attrs.contains("WEIGHTS_0") &&
                    reader.read(attrs["WEIGHTS_0"].get<int>(), f, comps) && comps >= 4) {
                    for (std::size_t i = 0; i + 3 < f.size(); i += comps)
                        gp.weights.push_back({f[i], f[i + 1], f[i + 2], f[i + 3]});
                }
                if (prim.contains("indices") &&
                    reader.read(prim["indices"].get<int>(), f, comps) && comps == 1) {
                    gp.indices.reserve(f.size());
                    for (float v : f) gp.indices.push_back(static_cast<std::uint32_t>(v));
                }
                for (const auto& target : prim.value("targets", json::array())) {
                    if (target.contains("POSITION") &&
                        reader.read(target["POSITION"].get<int>(), f, comps)) {
                        gp.morphPositions.push_back(asVec3(f, comps));
                    } else {
                        gp.morphPositions.emplace_back();
                    }
                }
                gm.primitives.push_back(std::move(gp));
            }
            out.meshes.push_back(std::move(gm));
        }
    }

    // ---- Nodes ----
    if (doc.contains("nodes")) {
        for (const auto& node : doc["nodes"]) {
            GltfNode gn;
            gn.name = node.value("name", "");
            gn.mesh = node.value("mesh", -1);
            gn.skin = node.value("skin", -1);
            if (node.contains("children")) {
                for (const auto& c : node["children"]) gn.children.push_back(c.get<int>());
            }
            if (node.contains("translation") && node["translation"].size() == 3) {
                gn.translation = {node["translation"][0], node["translation"][1],
                                  node["translation"][2]};
            }
            if (node.contains("scale") && node["scale"].size() == 3) {
                gn.scale = {node["scale"][0], node["scale"][1], node["scale"][2]};
            }
            if (node.contains("rotation") && node["rotation"].size() == 4) {
                gn.rotation = {node["rotation"][0], node["rotation"][1],
                               node["rotation"][2], node["rotation"][3]};
            }
            out.nodes.push_back(std::move(gn));
        }
    }

    // ---- Skins ----
    if (doc.contains("skins")) {
        for (const auto& skin : doc["skins"]) {
            GltfSkin gs;
            gs.skeleton = skin.value("skeleton", -1);
            if (skin.contains("joints")) {
                for (const auto& j : skin["joints"]) gs.joints.push_back(j.get<int>());
            }
            std::vector<float> f;
            int comps = 0;
            if (skin.contains("inverseBindMatrices") &&
                reader.read(skin["inverseBindMatrices"].get<int>(), f, comps) &&
                comps == 16) {
                for (std::size_t i = 0; i + 15 < f.size(); i += 16) {
                    Mat4 m;
                    for (int k = 0; k < 16; ++k)
                        m.m[static_cast<std::size_t>(k)] = f[i + k];
                    gs.inverseBind.push_back(m);
                }
            }
            out.skins.push_back(std::move(gs));
        }
    }

    // ---- Materials ----
    if (doc.contains("materials")) {
        for (const auto& mat : doc["materials"]) {
            GltfMaterial gm;
            gm.doubleSided = mat.value("doubleSided", false);
            gm.alphaMode = mat.value("alphaMode", "OPAQUE");
            gm.alphaCutoff = mat.value("alphaCutoff", 0.5F);
            if (mat.contains("pbrMetallicRoughness")) {
                const auto& pbr = mat["pbrMetallicRoughness"];
                if (pbr.contains("baseColorFactor") &&
                    pbr["baseColorFactor"].size() == 4) {
                    gm.baseColorFactor = {pbr["baseColorFactor"][0],
                                          pbr["baseColorFactor"][1],
                                          pbr["baseColorFactor"][2],
                                          pbr["baseColorFactor"][3]};
                }
                if (pbr.contains("baseColorTexture")) {
                    gm.baseColorTexture = pbr["baseColorTexture"].value("index", -1);
                }
            }
            out.materials.push_back(std::move(gm));
        }
    }

    // ---- Textures -> image ----
    if (doc.contains("textures")) {
        for (const auto& tex : doc["textures"]) {
            out.textureImage.push_back(tex.value("source", -1));
        }
    }

    // ---- Images (glb-embedded bufferView bytes only) ----
    if (doc.contains("images") && doc.contains("bufferViews")) {
        for (const auto& img : doc["images"]) {
            GltfImage gi;
            gi.mimeType = img.value("mimeType", "");
            if (img.contains("bufferView")) {
                const int bv = img["bufferView"].get<int>();
                const auto& views = doc["bufferViews"];
                if (bv >= 0 && static_cast<std::size_t>(bv) < views.size()) {
                    const auto& v = views[static_cast<std::size_t>(bv)];
                    const std::size_t off = v.value("byteOffset", 0U);
                    const std::size_t len = v.value("byteLength", 0U);
                    if (off + len <= glb.bin.size()) {
                        gi.bytes.assign(glb.bin.begin() + off,
                                        glb.bin.begin() + off + len);
                    }
                }
            }
            out.images.push_back(std::move(gi));
        }
    }

    // ---- Scene roots ----
    const int scene = doc.value("scene", 0);
    if (doc.contains("scenes") && scene >= 0 &&
        static_cast<std::size_t>(scene) < doc["scenes"].size()) {
        const auto& s = doc["scenes"][static_cast<std::size_t>(scene)];
        if (s.contains("nodes")) {
            for (const auto& n : s["nodes"]) out.sceneRoots.push_back(n.get<int>());
        }
    }
    if (out.sceneRoots.empty()) {
        for (int i = 0; i < static_cast<int>(out.nodes.size()); ++i)
            out.sceneRoots.push_back(i);
    }

    if (out.meshes.empty()) {
        return AppError{ErrorCode::ParseFailure, "glTF document has no meshes"};
    }
    return out;
}

}  // namespace creator::avatar::vrm
