#include "avatar/vrm/VrmRenderer.h"

#include "core/AppError.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>

namespace creator::avatar::vrm {
namespace {

using core::AppError;
using core::ErrorCode;

// Case-insensitive substring test for locating a head bone by node name.
bool nameLooksLikeHead(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(c)));
    return lower.find("head") != std::string::npos ||
           lower == "j_bip_c_head";
}

struct Vertex final {
    Vec3 world;
    Vec3 normal;
    Vec2 uv;
};

}  // namespace

VrmRenderer::VrmRenderer(GltfDocument document,
                         std::vector<DecodedTexture> textures,
                         std::uint32_t width, std::uint32_t height)
    : doc_(std::move(document)),
      textures_(std::move(textures)),
      width_(width),
      height_(height) {}

core::Result<std::unique_ptr<VrmRenderer>> VrmRenderer::open(
    GltfDocument document, std::vector<DecodedTexture> textures,
    std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U) {
        return AppError{ErrorCode::InvalidArgument, "VRM frame is empty"};
    }
    if (document.meshes.empty() || document.nodes.empty()) {
        return AppError{ErrorCode::ParseFailure, "VRM has no renderable meshes"};
    }
    std::unique_ptr<VrmRenderer> self{
        new VrmRenderer{std::move(document), std::move(textures), width, height}};

    // Parent index per node.
    self->parent_.assign(self->doc_.nodes.size(), -1);
    for (int i = 0; i < static_cast<int>(self->doc_.nodes.size()); ++i) {
        for (int child : self->doc_.nodes[static_cast<std::size_t>(i)].children) {
            if (child >= 0 && static_cast<std::size_t>(child) < self->parent_.size())
                self->parent_[static_cast<std::size_t>(child)] = i;
        }
    }
    // Head + upper-arm bones (by name; VRoid uses J_Bip_C_Head / J_Bip_L_UpperArm).
    for (int i = 0; i < static_cast<int>(self->doc_.nodes.size()); ++i) {
        std::string nm;
        for (char c : self->doc_.nodes[static_cast<std::size_t>(i)].name)
            nm.push_back(static_cast<char>(std::tolower(c)));
        if (self->headNode_ < 0 && nameLooksLikeHead(nm)) self->headNode_ = i;
        const bool isUpperArm =
            nm.find("upperarm") != std::string::npos ||
            nm.find("upper_arm") != std::string::npos;
        if (isUpperArm) {
            const bool left = nm.find("_l_") != std::string::npos ||
                              nm.find("left") != std::string::npos;
            const bool right = nm.find("_r_") != std::string::npos ||
                               nm.find("right") != std::string::npos;
            if (left && self->leftUpperArm_ < 0) self->leftUpperArm_ = i;
            else if (right && self->rightUpperArm_ < 0) self->rightUpperArm_ = i;
        }
    }
    return self;
}

core::Result<AvatarRenderFrame> VrmRenderer::render(
    core::TimestampNs timestamp, const VrmRenderParams& params) {
    const std::size_t nodeCount = doc_.nodes.size();

    // ---- Node world matrices (head bone gets the tracking rotation) ----------
    const Quat headRot =
        quatFromAxisAngle({0, 1, 0}, params.headYaw) *
        quatFromAxisAngle({1, 0, 0}, params.headPitch) *
        quatFromAxisAngle({0, 0, 1}, params.headRoll);
    std::vector<Mat4> local(nodeCount);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        local[i] = doc_.nodes[i].localMatrix();
        if (static_cast<int>(i) == headNode_)
            local[i] = local[i] * Mat4::rotation(headRot);
    }
    // Lower the arms from the T-pose to nearly straight down so only the head and
    // upper body are the subject. Signs are mirrored for left vs right.
    constexpr float kArmDown = 1.4F;
    if (leftUpperArm_ >= 0) {
        local[static_cast<std::size_t>(leftUpperArm_)] =
            local[static_cast<std::size_t>(leftUpperArm_)] *
            Mat4::rotation(quatFromAxisAngle({0, 0, 1}, kArmDown));
    }
    if (rightUpperArm_ >= 0) {
        local[static_cast<std::size_t>(rightUpperArm_)] =
            local[static_cast<std::size_t>(rightUpperArm_)] *
            Mat4::rotation(quatFromAxisAngle({0, 0, 1}, -kArmDown));
    }
    std::vector<Mat4> world(nodeCount);
    std::vector<char> done(nodeCount, 0);
    std::function<Mat4(int)> worldOf = [&](int i) -> Mat4 {
        if (done[static_cast<std::size_t>(i)]) return world[static_cast<std::size_t>(i)];
        const int p = parent_[static_cast<std::size_t>(i)];
        world[static_cast<std::size_t>(i)] =
            p < 0 ? local[static_cast<std::size_t>(i)]
                  : worldOf(p) * local[static_cast<std::size_t>(i)];
        done[static_cast<std::size_t>(i)] = 1;
        return world[static_cast<std::size_t>(i)];
    };
    for (int i = 0; i < static_cast<int>(nodeCount); ++i) worldOf(i);

    // ---- Gather world-space triangles ----------------------------------------
    std::vector<Vertex> verts;
    std::vector<std::array<std::uint32_t, 3>> tris;
    std::vector<int> triMaterial;

    for (std::size_t n = 0; n < nodeCount; ++n) {
        const auto& node = doc_.nodes[n];
        if (node.mesh < 0 ||
            static_cast<std::size_t>(node.mesh) >= doc_.meshes.size())
            continue;
        const auto& mesh = doc_.meshes[static_cast<std::size_t>(node.mesh)];
        // Precompute skin matrices for this node's skin, if any.
        std::vector<Mat4> skinMats;
        const bool skinned =
            node.skin >= 0 &&
            static_cast<std::size_t>(node.skin) < doc_.skins.size();
        if (skinned) {
            const auto& skin = doc_.skins[static_cast<std::size_t>(node.skin)];
            skinMats.resize(skin.joints.size());
            for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                const int jn = skin.joints[j];
                const Mat4 ib = j < skin.inverseBind.size() ? skin.inverseBind[j]
                                                            : Mat4::identity();
                skinMats[j] =
                    (jn >= 0 && static_cast<std::size_t>(jn) < nodeCount)
                        ? world[static_cast<std::size_t>(jn)] * ib
                        : ib;
            }
        }
        for (const auto& prim : mesh.primitives) {
            const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
            for (std::size_t v = 0; v < prim.positions.size(); ++v) {
                Vec3 pos = prim.positions[v];
                // Neutral morph (default mesh weights) so a model with a baked
                // expression still shows; live expressions come later.
                for (std::size_t t = 0;
                     t < prim.morphPositions.size() && t < mesh.weights.size(); ++t) {
                    if (v < prim.morphPositions[t].size())
                        pos = pos + prim.morphPositions[t][v] * mesh.weights[t];
                }
                Vec3 wpos;
                Vec3 wnorm = v < prim.normals.size() ? prim.normals[v] : Vec3{0, 0, 1};
                if (skinned && v < prim.joints.size() && v < prim.weights.size()) {
                    const auto& jj = prim.joints[v];
                    const Vec4 ww = prim.weights[v];
                    const std::array<float, 4> w{ww.x, ww.y, ww.z, ww.w};
                    Vec3 acc{0, 0, 0};
                    Vec3 accN{0, 0, 0};
                    float wsum = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (w[static_cast<std::size_t>(k)] <= 0.0F) continue;
                        const std::uint16_t ji = jj[static_cast<std::size_t>(k)];
                        if (ji >= skinMats.size()) continue;
                        acc = acc + skinMats[ji].transformPoint(pos) *
                                        w[static_cast<std::size_t>(k)];
                        accN = accN + skinMats[ji].transformDir(wnorm) *
                                          w[static_cast<std::size_t>(k)];
                        wsum += w[static_cast<std::size_t>(k)];
                    }
                    wpos = wsum > 0.0F ? acc * (1.0F / wsum) : world[n].transformPoint(pos);
                    wnorm = normalize(accN);
                } else {
                    wpos = world[n].transformPoint(pos);
                    wnorm = normalize(world[n].transformDir(wnorm));
                }
                // VRM models face -Z; the camera looks from +Z. Rotate 180° about
                // Y (x->-x, z->-z, a proper rotation so handedness is preserved
                // and the face is not mirrored) so the front faces the camera.
                wpos.x = -wpos.x;
                wpos.z = -wpos.z;
                wnorm.x = -wnorm.x;
                wnorm.z = -wnorm.z;
                verts.push_back({wpos, wnorm,
                                 v < prim.uvs.size() ? prim.uvs[v] : Vec2{0, 0}});
            }
            const auto& idx = prim.indices;
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                tris.push_back({base + idx[i], base + idx[i + 1], base + idx[i + 2]});
                triMaterial.push_back(prim.material);
            }
        }
    }
    if (verts.empty() || tris.empty()) {
        return AppError{ErrorCode::ParseFailure, "VRM produced no triangles"};
    }

    // ---- Framing: orthographic camera on the upper body (head + chest) --------
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
    Vec3 hi{std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
    for (const auto& v : verts) {
        lo.x = std::min(lo.x, v.world.x);
        lo.y = std::min(lo.y, v.world.y);
        lo.z = std::min(lo.z, v.world.z);
        hi.x = std::max(hi.x, v.world.x);
        hi.y = std::max(hi.y, v.world.y);
        hi.z = std::max(hi.z, v.world.z);
    }
    const float modelH = std::max(hi.y - lo.y, 1.0e-4F);
    // Keep the top ~40% (head + chest). VRM/VRoid models stand with +Y up, so the
    // head is near the top of the bounds.
    const float cropBottomY = hi.y - modelH * 0.42F;
    const float centreX = (lo.x + hi.x) * 0.5F;
    const float centreY = (hi.y + cropBottomY) * 0.5F;
    const float bandH = std::max(hi.y - cropBottomY, 1.0e-4F);
    const float bandW = std::max(hi.x - lo.x, 1.0e-4F);
    constexpr float kMargin = 1.12F;  // a little breathing room
    const float halfExtent =
        0.5F * std::max(bandH, bandW / (static_cast<float>(width_) /
                                        static_cast<float>(height_))) *
        kMargin;
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    const float halfH = halfExtent;
    const float halfW = halfExtent * aspect;

    std::vector<std::uint8_t> px(static_cast<std::size_t>(width_) * height_ * 4U, 0U);
    std::vector<float> depth(static_cast<std::size_t>(width_) * height_,
                             std::numeric_limits<float>::lowest());

    const Vec3 light = normalize({0.35F, 0.55F, 1.0F});
    const auto project = [&](Vec3 w) {
        const float sx = (w.x - centreX) / halfW * (static_cast<float>(width_) * 0.5F) +
                         static_cast<float>(width_) * 0.5F;
        const float sy = (centreY - w.y) / halfH * (static_cast<float>(height_) * 0.5F) +
                         static_cast<float>(height_) * 0.5F;
        return Vec2{sx, sy};
    };

    for (std::size_t t = 0; t < tris.size(); ++t) {
        const auto& tri = tris[t];
        const Vertex& a = verts[tri[0]];
        const Vertex& b = verts[tri[1]];
        const Vertex& c = verts[tri[2]];
        const Vec2 pa = project(a.world), pb = project(b.world), pc = project(c.world);
        const float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (std::abs(area) < 1.0e-6F) continue;
        // The camera looks toward -Z (from +Z); front faces have world +Z normals.
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({pa.x, pb.x, pc.x}))));
        const int maxX = std::min(static_cast<int>(width_) - 1,
                                  static_cast<int>(std::ceil(std::max({pa.x, pb.x, pc.x}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({pa.y, pb.y, pc.y}))));
        const int maxY = std::min(static_cast<int>(height_) - 1,
                                  static_cast<int>(std::ceil(std::max({pa.y, pb.y, pc.y}))));
        if (minX > maxX || minY > maxY) continue;

        const GltfMaterial mat =
            triMaterial[t] >= 0 &&
                    static_cast<std::size_t>(triMaterial[t]) < doc_.materials.size()
                ? doc_.materials[static_cast<std::size_t>(triMaterial[t])]
                : GltfMaterial{};
        const DecodedTexture* tex = nullptr;
        if (mat.baseColorTexture >= 0 &&
            static_cast<std::size_t>(mat.baseColorTexture) < doc_.textureImage.size()) {
            const int img = doc_.textureImage[static_cast<std::size_t>(mat.baseColorTexture)];
            if (img >= 0 && static_cast<std::size_t>(img) < textures_.size() &&
                textures_[static_cast<std::size_t>(img)].width > 0)
                tex = &textures_[static_cast<std::size_t>(img)];
        }

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float fx = static_cast<float>(x) + 0.5F;
                const float fy = static_cast<float>(y) + 0.5F;
                float w0 = ((pb.x - fx) * (pc.y - fy) - (pb.y - fy) * (pc.x - fx)) / area;
                float w1 = ((pc.x - fx) * (pa.y - fy) - (pc.y - fy) * (pa.x - fx)) / area;
                float w2 = 1.0F - w0 - w1;
                if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) continue;
                const float z = w0 * a.world.z + w1 * b.world.z + w2 * c.world.z;
                const std::size_t di =
                    static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x);
                if (z <= depth[di]) continue;  // larger Z = nearer the +Z camera

                float br = mat.baseColorFactor.x, bg = mat.baseColorFactor.y,
                      bb = mat.baseColorFactor.z, ba = mat.baseColorFactor.w;
                if (tex != nullptr) {
                    const float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
                    const float vv = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                    const auto wrap = [](float t) { return t - std::floor(t); };
                    const auto tx = static_cast<std::uint32_t>(
                        std::min(static_cast<float>(tex->width - 1U),
                                 wrap(u) * static_cast<float>(tex->width)));
                    const auto ty = static_cast<std::uint32_t>(
                        std::min(static_cast<float>(tex->height - 1U),
                                 wrap(vv) * static_cast<float>(tex->height)));
                    const std::size_t si = (static_cast<std::size_t>(ty) * tex->width + tx) * 4U;
                    br *= tex->rgba[si] / 255.0F;
                    bg *= tex->rgba[si + 1] / 255.0F;
                    bb *= tex->rgba[si + 2] / 255.0F;
                    ba *= tex->rgba[si + 3] / 255.0F;
                }
                if (mat.alphaMode == "MASK" && ba < mat.alphaCutoff) continue;
                if (mat.alphaMode == "OPAQUE") ba = 1.0F;
                if (ba <= 0.02F) continue;

                const Vec3 nrm = normalize(
                    {w0 * a.normal.x + w1 * b.normal.x + w2 * c.normal.x,
                     w0 * a.normal.y + w1 * b.normal.y + w2 * c.normal.y,
                     w0 * a.normal.z + w1 * b.normal.z + w2 * c.normal.z});
                // Two-step toon lighting so it reads as anime cel shading. A model
                // with no normals lands on the softer step (flat), which is fine.
                const float ndl = dot(nrm, light);
                const float shade = ndl > 0.25F ? 1.0F : 0.78F;
                const auto toByte = [](float v) {
                    return static_cast<std::uint8_t>(std::clamp(v, 0.0F, 1.0F) * 255.0F);
                };
                px[di * 4 + 0] = toByte(bb * shade);  // B
                px[di * 4 + 1] = toByte(bg * shade);  // G
                px[di * 4 + 2] = toByte(br * shade);  // R
                px[di * 4 + 3] = toByte(ba);
                depth[di] = z;
            }
        }
    }

    return AvatarRenderFrame::fromBgra(timestamp, width_, height_, width_ * 4U,
                                       std::move(px));
}

}  // namespace creator::avatar::vrm
