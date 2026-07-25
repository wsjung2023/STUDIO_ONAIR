#include "avatar/CharacterAvatarRenderer.h"

#include "avatar/PlaceholderAvatarRenderer.h"  // shared parameter names
#include "core/AppError.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

namespace creator::avatar {
namespace {

constexpr float kPi = 3.14159265358979323846F;

// A straight-alpha RGBA colour in [0,1]. Kept renderer-local; the frame is
// emitted as packed BGRA8 at the boundary.
struct Rgba final {
    float r{0}, g{0}, b{0}, a{1};
};

Rgba rgb(int r, int g, int b, float a = 1.0F) {
    return {static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F,
            static_cast<float>(b) / 255.0F, a};
}

Rgba mix(const Rgba& x, const Rgba& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t,
            x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

struct Pt final {
    float x{0}, y{0};
};

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

// An anti-aliased, alpha-blending software canvas. Everything is drawn at a
// supersampled resolution and box-downsampled into the packed BGRA8 output, so
// every edge — ellipses, triangles, hair polygons, whiskers — is smooth without
// per-primitive coverage maths. Deliberately free of any graphics library so
// the renderer stays in the Qt-free avatar layer.
class Canvas final {
public:
    Canvas(std::uint32_t outWidth, std::uint32_t outHeight, std::uint32_t ss)
        : outW_(outWidth),
          outH_(outHeight),
          ss_(std::max<std::uint32_t>(1, ss)),
          w_(outWidth * std::max<std::uint32_t>(1, ss)),
          h_(outHeight * std::max<std::uint32_t>(1, ss)),
          px_(static_cast<std::size_t>(w_) * h_) {}

    void clearTransparent() { std::fill(px_.begin(), px_.end(), Rgba{0, 0, 0, 0}); }

    void fillVerticalGradient(const Rgba& top, const Rgba& bottom) {
        for (std::uint32_t y = 0; y < h_; ++y) {
            const float t = h_ > 1 ? static_cast<float>(y) / (h_ - 1) : 0.0F;
            const Rgba row = mix(top, bottom, t);
            for (std::uint32_t x = 0; x < w_; ++x) px_[idx(x, y)] = row;
        }
    }

    // Straight-alpha "over" blend of one supersampled pixel.
    void blendPx(std::uint32_t x, std::uint32_t y, const Rgba& s) {
        if (x >= w_ || y >= h_ || s.a <= 0.0F) return;
        Rgba& d = px_[idx(x, y)];
        const float a = s.a;
        const float ia = 1.0F - a;
        const float outA = a + d.a * ia;
        if (outA <= 0.0F) {
            d = Rgba{0, 0, 0, 0};
            return;
        }
        d.r = (s.r * a + d.r * d.a * ia) / outA;
        d.g = (s.g * a + d.g * d.a * ia) / outA;
        d.b = (s.b * a + d.b * d.a * ia) / outA;
        d.a = outA;
    }

    void fillEllipse(float cx, float cy, float rx, float ry, const Rgba& c,
                     float rot = 0.0F) {
        cx *= ss_; cy *= ss_; rx *= ss_; ry *= ss_;
        if (rx < 0.3F || ry < 0.3F) return;
        const float cs = std::cos(-rot), sn = std::sin(-rot);
        const float rr = std::max(rx, ry) + 1.0F;
        const int minX = static_cast<int>(std::floor(cx - rr));
        const int maxX = static_cast<int>(std::ceil(cx + rr));
        const int minY = static_cast<int>(std::floor(cy - rr));
        const int maxY = static_cast<int>(std::ceil(cy + rr));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                if (x < 0 || y < 0) continue;
                const float dx = static_cast<float>(x) + 0.5F - cx;
                const float dy = static_cast<float>(y) + 0.5F - cy;
                const float lx = dx * cs - dy * sn;
                const float ly = dx * sn + dy * cs;
                const float d = (lx * lx) / (rx * rx) + (ly * ly) / (ry * ry);
                if (d <= 1.0F) {
                    Rgba s = c;
                    blendPx(static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), s);
                }
            }
        }
    }

    void fillRoundedRect(float x0, float y0, float x1, float y1, float radius,
                         const Rgba& c) {
        x0 *= ss_; y0 *= ss_; x1 *= ss_; y1 *= ss_; radius *= ss_;
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        radius = std::min(radius, std::min((x1 - x0) * 0.5F, (y1 - y0) * 0.5F));
        const int minX = static_cast<int>(std::floor(x0));
        const int maxX = static_cast<int>(std::ceil(x1));
        const int minY = static_cast<int>(std::floor(y0));
        const int maxY = static_cast<int>(std::ceil(y1));
        for (int y = std::max(0, minY); y <= maxY; ++y) {
            for (int x = std::max(0, minX); x <= maxX; ++x) {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                if (px < x0 || px > x1 || py < y0 || py > y1) continue;
                // Distance into the rounded corners.
                const float qx = std::max(std::max(x0 + radius - px, px - (x1 - radius)), 0.0F);
                const float qy = std::max(std::max(y0 + radius - py, py - (y1 - radius)), 0.0F);
                if (qx * qx + qy * qy <= radius * radius || radius < 0.5F) {
                    blendPx(static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), c);
                }
            }
        }
    }

    // Even-odd scanline fill of an arbitrary simple polygon (points in output
    // space). Used for ears, hair, cheek ruffs, muzzles.
    void fillPolygon(const std::vector<Pt>& poly, const Rgba& c) {
        if (poly.size() < 3) return;
        std::vector<Pt> p(poly.size());
        float minY = 1e30F, maxY = -1e30F;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            p[i] = {poly[i].x * ss_, poly[i].y * ss_};
            minY = std::min(minY, p[i].y);
            maxY = std::max(maxY, p[i].y);
        }
        const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
        const int y1 = std::min(static_cast<int>(h_) - 1,
                                static_cast<int>(std::ceil(maxY)));
        std::vector<float> xs;
        for (int y = y0; y <= y1; ++y) {
            const float sy = static_cast<float>(y) + 0.5F;
            xs.clear();
            for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
                const float yi = p[i].y, yj = p[j].y;
                if ((yi <= sy && yj > sy) || (yj <= sy && yi > sy)) {
                    const float t = (sy - yi) / (yj - yi);
                    xs.push_back(p[i].x + t * (p[j].x - p[i].x));
                }
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int xa = std::max(0, static_cast<int>(std::floor(xs[k] + 0.5F)));
                const int xb = std::min(static_cast<int>(w_) - 1,
                                        static_cast<int>(std::ceil(xs[k + 1] - 0.5F)));
                for (int x = xa; x <= xb; ++x) {
                    blendPx(static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), c);
                }
            }
        }
    }

    // A stroked line as a rounded capsule.
    void strokeLine(Pt a, Pt b, float width, const Rgba& c) {
        const float hw = width * 0.5F;
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4F) {
            fillEllipse(a.x, a.y, hw, hw, c);
            return;
        }
        const float nx = -dy / len * hw, ny = dx / len * hw;
        fillPolygon({{a.x + nx, a.y + ny},
                     {b.x + nx, b.y + ny},
                     {b.x - nx, b.y - ny},
                     {a.x - nx, a.y - ny}},
                    c);
        fillEllipse(a.x, a.y, hw, hw, c);
        fillEllipse(b.x, b.y, hw, hw, c);
    }

    // Downsample the supersampled buffer into packed BGRA8 (premultiplied
    // averaging so edges over transparency stay clean).
    [[nodiscard]] std::vector<std::uint8_t> toBgra() const {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(outW_) * outH_ * 4U);
        const float inv = 1.0F / static_cast<float>(ss_ * ss_);
        for (std::uint32_t oy = 0; oy < outH_; ++oy) {
            for (std::uint32_t ox = 0; ox < outW_; ++ox) {
                float pr = 0, pg = 0, pb = 0, pa = 0;
                for (std::uint32_t sy = 0; sy < ss_; ++sy) {
                    for (std::uint32_t sx = 0; sx < ss_; ++sx) {
                        const Rgba& s = px_[idx(ox * ss_ + sx, oy * ss_ + sy)];
                        pr += s.r * s.a;
                        pg += s.g * s.a;
                        pb += s.b * s.a;
                        pa += s.a;
                    }
                }
                const float a = pa * inv;
                float r = 0, g = 0, b = 0;
                if (pa > 0.0F) {
                    r = pr / pa;
                    g = pg / pa;
                    b = pb / pa;
                }
                const auto o = (static_cast<std::size_t>(oy) * outW_ + ox) * 4U;
                out[o + 0] = to8(b);
                out[o + 1] = to8(g);
                out[o + 2] = to8(r);
                out[o + 3] = to8(a);
            }
        }
        return out;
    }

private:
    [[nodiscard]] std::size_t idx(std::uint32_t x, std::uint32_t y) const {
        return static_cast<std::size_t>(y) * w_ + x;
    }
    static std::uint8_t to8(float v) {
        return static_cast<std::uint8_t>(clampf(v, 0.0F, 1.0F) * 255.0F + 0.5F);
    }

    std::uint32_t outW_, outH_, ss_, w_, h_;
    std::vector<Rgba> px_;
};

// Local face frame: maps rig-local coordinates (in units of the face radius R,
// origin at the face centre) to canvas pixels, applying head roll. Yaw/pitch are
// folded into the centre plus a small feature parallax by the caller.
struct Pose final {
    float cx{0}, cy{0}, R{0}, roll{0};
    [[nodiscard]] Pt at(float lx, float ly) const {
        const float x = lx * R, y = ly * R;
        const float c = std::cos(roll), s = std::sin(roll);
        return {cx + x * c - y * s, cy + x * s + y * c};
    }
};

float valueOf(std::span<const AvatarParameterValue> parameters,
              std::string_view name, float fallback) {
    for (const auto& p : parameters) {
        if (p.modelParameter == name) return p.value;
    }
    return fallback;
}

// The nine mapped, clamped expression inputs plus the derived head geometry a
// character draw function consumes.
struct Rig final {
    float eyeOpenL{1}, eyeOpenR{1};
    float browUpL{0}, browUpR{0};
    float mouthOpen{0}, mouthWide{0};
    float yaw{0}, pitch{0}, roll{0};
    Pose pose;
    float gaze{0};  // horizontal pupil offset in R units
};

// ---- Shared rig layers -----------------------------------------------------

// One eye: sclera, iris, pupil, highlight, then the eyelid closing over it and
// an eyebrow that raises with browUp. `almond` narrows the eye vertically for a
// more animal/serious look.
void drawEye(Canvas& cv, const Rig& r, float side /* -1 left, +1 right */,
             float openness, float browUp, const Rgba& skin, const Rgba& iris,
             const Rgba& brow, float eyeW, float eyeH, float almond,
             bool drawBrow, const Rgba& sclera = rgb(255, 253, 250)) {
    const Pose& p = r.pose;
    const float ex = side * 0.42F;
    const float ey = -0.10F;
    const Pt e = p.at(ex, ey);
    const float rx = eyeW * p.R;
    const float ry = eyeH * p.R * almond;

    // Sclera (white by default; a dark colour gives glossy "black" eyes).
    cv.fillEllipse(e.x, e.y, rx, ry, sclera, p.roll);
    // Iris + pupil, shifted by gaze/pitch.
    const float gx = clampf(r.gaze, -0.55F, 0.55F) * rx;
    const float gy = clampf(-r.pitch * 0.35F, -0.4F, 0.4F) * ry;
    const float irisR = std::min(rx, ry) * 0.82F;
    cv.fillEllipse(e.x + gx, e.y + gy, irisR, irisR, iris, p.roll);
    cv.fillEllipse(e.x + gx, e.y + gy, irisR * 0.52F, irisR * 0.52F,
                   rgb(20, 18, 24), p.roll);
    // Catchlight.
    cv.fillEllipse(e.x + gx - irisR * 0.3F, e.y + gy - irisR * 0.35F,
                   irisR * 0.26F, irisR * 0.26F, rgb(255, 255, 255, 0.9F), p.roll);

    // Upper eyelid closing over the eye: a skin-coloured cap descending from the
    // top of the eye by (1-openness). At openness 0 it covers the whole eye.
    const float closed = clampf(1.0F - openness, 0.0F, 1.0F);
    if (closed > 0.02F) {
        const float lidH = ry * 2.0F * closed;
        const Pt lidC = p.at(ex, ey - ry / p.R + (lidH * 0.5F) / p.R);
        cv.fillEllipse(lidC.x, lidC.y, rx * 1.08F, lidH * 0.5F + ry * 0.08F, skin,
                       p.roll);
        // Lash line at the lid's lower edge.
        const Pt l0 = p.at(ex - eyeW * 1.02F, ey - ry / p.R + lidH / p.R);
        const Pt l1 = p.at(ex + eyeW * 1.02F, ey - ry / p.R + lidH / p.R);
        cv.strokeLine({l0.x, l0.y}, {l1.x, l1.y}, p.R * 0.03F, rgb(70, 52, 60));
    } else {
        // Subtle upper lash line when open.
        const Pt l0 = p.at(ex - eyeW * 1.0F, ey - ry / p.R);
        const Pt l1 = p.at(ex + eyeW * 1.0F, ey - ry / p.R * 0.9F);
        cv.strokeLine({l0.x, l0.y}, {l1.x, l1.y}, p.R * 0.028F, rgb(70, 52, 60, 0.8F));
    }

    if (drawBrow) {
        const float browY = ey - eyeH * almond - 0.14F - browUp * 0.10F;
        const Pt b0 = p.at(ex - eyeW * 1.05F, browY + 0.02F);
        const Pt b1 = p.at(ex, browY - 0.02F);
        const Pt b2 = p.at(ex + eyeW * 1.05F, browY + 0.02F);
        cv.strokeLine({b0.x, b0.y}, {b1.x, b1.y}, p.R * 0.055F, brow);
        cv.strokeLine({b1.x, b1.y}, {b2.x, b2.y}, p.R * 0.055F, brow);
    }
}

// Rounded cheek blush.
void drawBlush(Canvas& cv, const Rig& r, const Rgba& c) {
    const Pose& p = r.pose;
    cv.fillEllipse(p.at(-0.5F, 0.22F).x, p.at(-0.5F, 0.22F).y, p.R * 0.16F,
                   p.R * 0.10F, c, p.roll);
    cv.fillEllipse(p.at(0.5F, 0.22F).x, p.at(0.5F, 0.22F).y, p.R * 0.16F,
                   p.R * 0.10F, c, p.roll);
}

// ---- Characters ------------------------------------------------------------

void drawHuman(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(248, 214, 186);
    const Rgba skinShade = rgb(232, 190, 160);
    const Rgba hair = rgb(97, 62, 71);
    const Rgba hairHi = rgb(126, 84, 94);

    // Shoulders / bust (clothing) behind the head.
    cv.fillRoundedRect(p.cx - p.R * 1.35F, p.cy + p.R * 1.05F, p.cx + p.R * 1.35F,
                       p.cy + p.R * 2.4F, p.R * 0.5F, rgb(86, 130, 168));
    // Neck.
    cv.fillRoundedRect(p.cx - p.R * 0.26F, p.cy + p.R * 0.6F, p.cx + p.R * 0.26F,
                       p.cy + p.R * 1.2F, p.R * 0.2F, skinShade);

    // Back hair (frames the face).
    cv.fillEllipse(p.cx, p.cy - p.R * 0.05F, p.R * 1.02F, p.R * 1.18F, hair,
                   p.roll);
    // Ears.
    cv.fillEllipse(p.at(-0.98F, 0.06F).x, p.at(-0.98F, 0.06F).y, p.R * 0.16F,
                   p.R * 0.22F, skin, p.roll);
    cv.fillEllipse(p.at(0.98F, 0.06F).x, p.at(0.98F, 0.06F).y, p.R * 0.16F,
                   p.R * 0.22F, skin, p.roll);
    // Face.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.86F, p.R, skin, p.roll);
    // Soft jaw shading.
    cv.fillEllipse(p.at(0, 0.55F).x, p.at(0, 0.55F).y, p.R * 0.5F, p.R * 0.3F,
                   mix(skin, skinShade, 0.5F), p.roll);

    drawBlush(cv, r, rgb(255, 150, 150, 0.5F));

    // Front hair: a fringe of soft bangs over the forehead.
    auto bang = [&](float x0, float x1, float tipY) {
        cv.fillPolygon({p.at(x0, -0.66F), p.at(x1, -0.66F),
                        p.at((x0 + x1) * 0.5F, tipY)},
                       hair);
    };
    cv.fillEllipse(p.cx, p.cy - p.R * 0.62F, p.R * 0.92F, p.R * 0.5F, hair, p.roll);
    bang(-0.95F, -0.35F, 0.02F);
    bang(-0.5F, 0.15F, 0.14F);
    bang(0.05F, 0.6F, 0.05F);
    bang(0.45F, 0.98F, -0.04F);
    // Hair highlight streak, kept up in the crown so it reads as a soft sheen
    // rather than a mark on the forehead.
    cv.fillEllipse(p.at(-0.34F, -0.78F).x, p.at(-0.34F, -0.78F).y, p.R * 0.08F,
                   p.R * 0.16F, hairHi, p.roll);

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(120, 78, 52), hair,
            0.19F, 0.16F, 1.0F, true);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(120, 78, 52), hair,
            0.19F, 0.16F, 1.0F, true);

    // Nose (tiny shadow).
    const Pt n = p.at(0.0F, 0.2F);
    cv.fillEllipse(n.x, n.y, p.R * 0.05F, p.R * 0.07F, skinShade, p.roll);

    // Mouth.
    const Pt m = p.at(0.0F, 0.5F);
    const float mw = p.R * (0.16F + 0.16F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.28F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(m.x, m.y, mw, mo, rgb(150, 60, 74), p.roll);          // lips
        cv.fillEllipse(m.x, m.y, mw * 0.82F, mo * 0.82F, rgb(92, 34, 48), p.roll);  // mouth
        cv.fillEllipse(m.x, m.y + mo * 0.4F, mw * 0.55F, mo * 0.45F,
                       rgb(224, 108, 120), p.roll);                          // tongue
    } else {
        cv.strokeLine({p.at(-0.14F, 0.5F).x, p.at(-0.14F, 0.5F).y},
                      {p.at(0.14F, 0.5F).x, p.at(0.14F, 0.5F).y}, p.R * 0.045F,
                      rgb(176, 86, 92));
    }
}

void drawCat(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba fur = rgb(247, 197, 130);
    const Rgba furDk = rgb(226, 168, 96);
    const Rgba innerEar = rgb(255, 190, 196);
    const Rgba white = rgb(255, 252, 246);

    // Small body, tucked behind the head so it reads as shoulders.
    cv.fillRoundedRect(p.cx - p.R * 1.0F, p.cy + p.R * 0.8F, p.cx + p.R * 1.0F,
                       p.cy + p.R * 2.2F, p.R * 0.6F, furDk);

    // Ears (triangles) with pink inner.
    auto ear = [&](float sign) {
        const Pt a = p.at(sign * 0.35F, -0.82F);
        const Pt b = p.at(sign * 0.95F, -1.28F);
        const Pt c = p.at(sign * 0.98F, -0.55F);
        cv.fillPolygon({a, b, c}, fur);
        const Pt ia = p.at(sign * 0.5F, -0.78F);
        const Pt ib = p.at(sign * 0.83F, -1.08F);
        const Pt ic = p.at(sign * 0.86F, -0.62F);
        cv.fillPolygon({ia, ib, ic}, innerEar);
    };
    ear(-1.0F);
    ear(1.0F);

    // Head (round) + cheek fluff.
    cv.fillEllipse(p.at(-0.7F, 0.35F).x, p.at(-0.7F, 0.35F).y, p.R * 0.34F,
                   p.R * 0.3F, fur, p.roll);
    cv.fillEllipse(p.at(0.7F, 0.35F).x, p.at(0.7F, 0.35F).y, p.R * 0.34F,
                   p.R * 0.3F, fur, p.roll);
    cv.fillEllipse(p.cx, p.cy, p.R * 0.98F, p.R * 0.92F, fur, p.roll);
    // Forehead marking.
    cv.fillPolygon({p.at(-0.28F, -0.9F), p.at(0.28F, -0.9F), p.at(0.0F, -0.2F)},
                   furDk);

    drawBlush(cv, r, rgb(255, 148, 150, 0.45F));

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, fur, rgb(96, 176, 108),
            rgb(150, 110, 70), 0.2F, 0.2F, 1.0F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, fur, rgb(96, 176, 108),
            rgb(150, 110, 70), 0.2F, 0.2F, 1.0F, false);

    // Muzzle (light) with nose + mouth.
    cv.fillEllipse(p.at(0, 0.34F).x, p.at(0, 0.34F).y, p.R * 0.34F, p.R * 0.26F,
                   white, p.roll);
    const Pt nose = p.at(0.0F, 0.2F);
    cv.fillPolygon({p.at(-0.09F, 0.16F), p.at(0.09F, 0.16F), p.at(0.0F, 0.28F)},
                   rgb(232, 120, 138));
    // Cat "w" mouth, opening downward.
    const float mo = p.R * (0.0F + 0.3F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0, 0.42F).x, p.at(0, 0.42F).y + mo * 0.4F,
                       p.R * (0.12F + 0.08F * r.mouthWide), mo * 0.7F,
                       rgb(150, 66, 82), p.roll);
    } else {
        cv.strokeLine({nose.x, nose.y}, {p.at(0, 0.34F).x, p.at(0, 0.34F).y},
                      p.R * 0.03F, rgb(150, 100, 80));
        cv.strokeLine({p.at(0, 0.34F).x, p.at(0, 0.34F).y},
                      {p.at(-0.16F, 0.44F).x, p.at(-0.16F, 0.44F).y}, p.R * 0.03F,
                      rgb(150, 100, 80));
        cv.strokeLine({p.at(0, 0.34F).x, p.at(0, 0.34F).y},
                      {p.at(0.16F, 0.44F).x, p.at(0.16F, 0.44F).y}, p.R * 0.03F,
                      rgb(150, 100, 80));
    }

    // Whiskers.
    const Rgba wk = rgb(255, 255, 255, 0.85F);
    for (float dy : {-0.04F, 0.06F, 0.16F}) {
        cv.strokeLine({p.at(-0.28F, 0.24F + dy).x, p.at(-0.28F, 0.24F + dy).y},
                      {p.at(-0.95F, 0.18F + dy * 1.5F).x, p.at(-0.95F, 0.18F + dy * 1.5F).y},
                      p.R * 0.016F, wk);
        cv.strokeLine({p.at(0.28F, 0.24F + dy).x, p.at(0.28F, 0.24F + dy).y},
                      {p.at(0.95F, 0.18F + dy * 1.5F).x, p.at(0.95F, 0.18F + dy * 1.5F).y},
                      p.R * 0.016F, wk);
    }
}

void drawFox(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba fur = rgb(236, 130, 54);
    const Rgba furDk = rgb(206, 100, 36);
    const Rgba white = rgb(255, 250, 244);
    const Rgba black = rgb(38, 32, 34);
    const Rgba innerEar = rgb(255, 205, 180);

    // Body, tucked behind the head so it reads as shoulders.
    cv.fillRoundedRect(p.cx - p.R * 0.95F, p.cy + p.R * 0.82F, p.cx + p.R * 0.95F,
                       p.cy + p.R * 2.2F, p.R * 0.6F, furDk);

    // Tall pointed ears with black tips.
    auto ear = [&](float sign) {
        cv.fillPolygon({p.at(sign * 0.3F, -0.8F), p.at(sign * 0.78F, -1.55F),
                        p.at(sign * 0.95F, -0.5F)},
                       fur);
        cv.fillPolygon({p.at(sign * 0.62F, -1.16F), p.at(sign * 0.78F, -1.55F),
                        p.at(sign * 0.9F, -1.02F)},
                       black);
        cv.fillPolygon({p.at(sign * 0.45F, -0.78F), p.at(sign * 0.72F, -1.2F),
                        p.at(sign * 0.82F, -0.62F)},
                       innerEar);
    };
    ear(-1.0F);
    ear(1.0F);

    // Head (orange upper).
    cv.fillEllipse(p.cx, p.cy - p.R * 0.05F, p.R * 0.94F, p.R * 0.98F, fur, p.roll);

    // White cheek ruff (pointed) + muzzle — the fox's signature.
    cv.fillPolygon({p.at(-0.9F, 0.0F), p.at(0.0F, 0.15F), p.at(-0.2F, 0.85F),
                    p.at(-0.55F, 0.72F), p.at(-0.95F, 0.45F)},
                   white);
    cv.fillPolygon({p.at(0.9F, 0.0F), p.at(0.0F, 0.15F), p.at(0.2F, 0.85F),
                    p.at(0.55F, 0.72F), p.at(0.95F, 0.45F)},
                   white);
    cv.fillEllipse(p.at(0, 0.35F).x, p.at(0, 0.35F).y, p.R * 0.4F, p.R * 0.42F,
                   white, p.roll);

    drawBlush(cv, r, rgb(255, 150, 120, 0.4F));

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, fur, rgb(150, 96, 40),
            rgb(120, 70, 40), 0.17F, 0.16F, 0.92F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, fur, rgb(150, 96, 40),
            rgb(120, 70, 40), 0.17F, 0.16F, 0.92F, false);

    // Black nose at the muzzle tip.
    const Pt nose = p.at(0.0F, 0.34F);
    cv.fillEllipse(nose.x, nose.y, p.R * 0.11F, p.R * 0.09F, black, p.roll);
    cv.fillEllipse(nose.x - p.R * 0.03F, nose.y - p.R * 0.03F, p.R * 0.035F,
                   p.R * 0.03F, rgb(120, 120, 128, 0.8F), p.roll);

    // Mouth below the nose.
    const float mo = p.R * (0.0F + 0.28F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0, 0.52F).x, p.at(0, 0.52F).y,
                       p.R * (0.1F + 0.08F * r.mouthWide), mo * 0.8F,
                       rgb(140, 60, 70), p.roll);
    } else {
        cv.strokeLine({nose.x, nose.y}, {p.at(0, 0.5F).x, p.at(0, 0.5F).y},
                      p.R * 0.028F, rgb(120, 80, 70));
        cv.strokeLine({p.at(0, 0.5F).x, p.at(0, 0.5F).y},
                      {p.at(-0.14F, 0.56F).x, p.at(-0.14F, 0.56F).y}, p.R * 0.028F,
                      rgb(120, 80, 70));
        cv.strokeLine({p.at(0, 0.5F).x, p.at(0, 0.5F).y},
                      {p.at(0.14F, 0.56F).x, p.at(0.14F, 0.56F).y}, p.R * 0.028F,
                      rgb(120, 80, 70));
    }
}

// Small helper: a subtle brow mark (for the animal faces) that rises with the
// browUp channel, so browUp visibly drives even the brow-less characters.
void drawBrowDots(Canvas& cv, const Rig& r, const Rgba& c) {
    const Pose& p = r.pose;
    for (float s : {-1.0F, 1.0F}) {
        const float bu = s < 0.0F ? r.browUpL : r.browUpR;
        const Pt bp = p.at(s * 0.42F, -0.34F - bu * 0.12F);
        cv.fillEllipse(bp.x, bp.y, p.R * 0.11F, p.R * 0.045F, c, p.roll);
    }
}

void drawManMid(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(230, 190, 158);
    const Rgba skinShade = rgb(206, 164, 132);
    const Rgba hair = rgb(70, 62, 58);
    const Rgba hairHi = rgb(104, 94, 88);
    const Rgba stubble = rgb(96, 84, 84, 0.32F);

    // Collared shirt shoulders.
    cv.fillRoundedRect(p.cx - p.R * 1.4F, p.cy + p.R * 1.05F, p.cx + p.R * 1.4F,
                       p.cy + p.R * 2.4F, p.R * 0.4F, rgb(92, 104, 120));
    cv.fillPolygon({p.at(-0.5F, 1.02F), p.at(0.0F, 1.66F), p.at(0.5F, 1.02F)},
                   rgb(232, 236, 240));
    // Neck.
    cv.fillRoundedRect(p.cx - p.R * 0.3F, p.cy + p.R * 0.6F, p.cx + p.R * 0.3F,
                       p.cy + p.R * 1.2F, p.R * 0.2F, skinShade);
    // Ears.
    cv.fillEllipse(p.at(-0.98F, 0.08F).x, p.at(-0.98F, 0.08F).y, p.R * 0.16F,
                   p.R * 0.22F, skin, p.roll);
    cv.fillEllipse(p.at(0.98F, 0.08F).x, p.at(0.98F, 0.08F).y, p.R * 0.16F,
                   p.R * 0.22F, skin, p.roll);
    // Face with a heavier, squarer jaw.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.9F, p.R * 1.0F, skin, p.roll);
    cv.fillEllipse(p.at(0.0F, 0.44F).x, p.at(0.0F, 0.44F).y, p.R * 0.74F,
                   p.R * 0.52F, skin, p.roll);
    // Stubble shadow on the lower jaw/chin.
    cv.fillEllipse(p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y, p.R * 0.64F,
                   p.R * 0.36F, stubble, p.roll);
    cv.fillEllipse(p.at(-0.5F, 0.34F).x, p.at(-0.5F, 0.34F).y, p.R * 0.22F,
                   p.R * 0.2F, stubble, p.roll);
    cv.fillEllipse(p.at(0.5F, 0.34F).x, p.at(0.5F, 0.34F).y, p.R * 0.22F,
                   p.R * 0.2F, stubble, p.roll);
    // Side-parted hair: a cap plus a sweeping asymmetric fringe.
    cv.fillEllipse(p.at(-0.04F, -0.66F).x, p.at(-0.04F, -0.66F).y, p.R * 0.95F,
                   p.R * 0.5F, hair, p.roll);
    cv.fillPolygon({p.at(-0.95F, -0.82F), p.at(0.38F, -0.96F), p.at(0.2F, -0.4F),
                    p.at(-0.95F, -0.28F)},
                   hair);
    cv.fillEllipse(p.at(-0.5F, -0.8F).x, p.at(-0.5F, -0.8F).y, p.R * 0.1F,
                   p.R * 0.18F, hairHi, p.roll);

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(96, 72, 52), hair,
            0.18F, 0.14F, 1.0F, true);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(96, 72, 52), hair,
            0.18F, 0.14F, 1.0F, true);
    // Heavier brows overlaid on the drawEye brow line.
    for (float s : {-1.0F, 1.0F}) {
        const float bu = s < 0.0F ? r.browUpL : r.browUpR;
        const float browY = -0.38F - bu * 0.10F;
        cv.strokeLine({p.at(s * 0.62F, browY + 0.02F).x, p.at(s * 0.62F, browY + 0.02F).y},
                      {p.at(s * 0.2F, browY - 0.02F).x, p.at(s * 0.2F, browY - 0.02F).y},
                      p.R * 0.06F, hair);
    }
    // Nose.
    const Pt n = p.at(0.0F, 0.22F);
    cv.fillEllipse(n.x, n.y, p.R * 0.07F, p.R * 0.09F, skinShade, p.roll);
    // Mouth.
    const Pt m = p.at(0.0F, 0.5F);
    const float mw = p.R * (0.15F + 0.16F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.26F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(m.x, m.y, mw, mo, rgb(150, 76, 78), p.roll);
        cv.fillEllipse(m.x, m.y, mw * 0.82F, mo * 0.8F, rgb(96, 40, 46), p.roll);
    } else {
        cv.strokeLine({p.at(-0.15F, 0.5F).x, p.at(-0.15F, 0.5F).y},
                      {p.at(0.15F, 0.5F).x, p.at(0.15F, 0.5F).y}, p.R * 0.045F,
                      rgb(150, 84, 82));
    }
}

void drawWoman(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(250, 220, 198);
    const Rgba skinShade = rgb(232, 196, 170);
    const Rgba hair = rgb(120, 78, 52);
    const Rgba hairHi = rgb(160, 114, 80);
    const Rgba lip = rgb(214, 96, 108);

    // Long hair fanning out behind the head and shoulders.
    cv.fillEllipse(p.cx, p.cy + p.R * 0.45F, p.R * 1.28F, p.R * 1.72F, hair, p.roll);
    // Top / bust.
    cv.fillRoundedRect(p.cx - p.R * 1.2F, p.cy + p.R * 1.1F, p.cx + p.R * 1.2F,
                       p.cy + p.R * 2.4F, p.R * 0.6F, rgb(198, 122, 152));
    // Neck.
    cv.fillRoundedRect(p.cx - p.R * 0.22F, p.cy + p.R * 0.6F, p.cx + p.R * 0.22F,
                       p.cy + p.R * 1.15F, p.R * 0.18F, skinShade);
    // Face — soft, tapered chin.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.8F, p.R * 0.98F, skin, p.roll);
    cv.fillEllipse(p.at(0.0F, 0.46F).x, p.at(0.0F, 0.46F).y, p.R * 0.46F,
                   p.R * 0.46F, skin, p.roll);
    drawBlush(cv, r, rgb(255, 150, 156, 0.5F));
    // Long side locks framing the face.
    cv.fillPolygon({p.at(-1.08F, -0.5F), p.at(-0.72F, -0.9F), p.at(-0.56F, 0.95F),
                    p.at(-1.18F, 0.72F)},
                   hair);
    cv.fillPolygon({p.at(1.08F, -0.5F), p.at(0.72F, -0.9F), p.at(0.56F, 0.95F),
                    p.at(1.18F, 0.72F)},
                   hair);
    // Centre-parted fringe.
    cv.fillEllipse(p.cx, p.cy - p.R * 0.6F, p.R * 0.9F, p.R * 0.5F, hair, p.roll);
    cv.fillPolygon({p.at(-0.92F, -0.62F), p.at(-0.06F, -0.66F), p.at(-0.5F, 0.06F)},
                   hair);
    cv.fillPolygon({p.at(0.92F, -0.62F), p.at(0.06F, -0.66F), p.at(0.5F, 0.06F)},
                   hair);
    cv.fillEllipse(p.at(-0.42F, -0.78F).x, p.at(-0.42F, -0.78F).y, p.R * 0.08F,
                   p.R * 0.18F, hairHi, p.roll);

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(122, 84, 60), hair,
            0.2F, 0.18F, 1.0F, true);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(122, 84, 60), hair,
            0.2F, 0.18F, 1.0F, true);
    // Long outer lashes.
    for (float s : {-1.0F, 1.0F}) {
        const Pt o = p.at(s * 0.62F, -0.16F);
        cv.strokeLine({o.x, o.y}, {p.at(s * 0.82F, -0.26F).x, p.at(s * 0.82F, -0.26F).y},
                      p.R * 0.03F, rgb(58, 42, 48));
        cv.strokeLine({o.x, o.y}, {p.at(s * 0.8F, -0.12F).x, p.at(s * 0.8F, -0.12F).y},
                      p.R * 0.024F, rgb(58, 42, 48));
    }
    // Nose.
    const Pt n = p.at(0.0F, 0.2F);
    cv.fillEllipse(n.x, n.y, p.R * 0.045F, p.R * 0.06F, skinShade, p.roll);
    // Lips.
    const Pt m = p.at(0.0F, 0.48F);
    const float mw = p.R * (0.15F + 0.14F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.26F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(m.x, m.y, mw, mo, lip, p.roll);
        cv.fillEllipse(m.x, m.y, mw * 0.8F, mo * 0.78F, rgb(120, 46, 60), p.roll);
        cv.fillEllipse(m.x, m.y + mo * 0.42F, mw * 0.55F, mo * 0.45F,
                       rgb(226, 120, 130), p.roll);
    } else {
        cv.fillEllipse(m.x, m.y, mw * 0.92F, p.R * 0.06F, lip, p.roll);
    }
}

void drawBoy(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(252, 216, 184);
    const Rgba skinShade = rgb(234, 190, 158);
    const Rgba hair = rgb(74, 54, 42);
    const Rgba hairHi = rgb(108, 82, 64);

    cv.fillRoundedRect(p.cx - p.R * 1.2F, p.cy + p.R * 1.0F, p.cx + p.R * 1.2F,
                       p.cy + p.R * 2.3F, p.R * 0.5F, rgb(84, 172, 118));
    cv.fillRoundedRect(p.cx - p.R * 0.24F, p.cy + p.R * 0.62F, p.cx + p.R * 0.24F,
                       p.cy + p.R * 1.1F, p.R * 0.2F, skinShade);
    // Ears.
    cv.fillEllipse(p.at(-0.92F, 0.08F).x, p.at(-0.92F, 0.08F).y, p.R * 0.15F,
                   p.R * 0.19F, skin, p.roll);
    cv.fillEllipse(p.at(0.92F, 0.08F).x, p.at(0.92F, 0.08F).y, p.R * 0.15F,
                   p.R * 0.19F, skin, p.roll);
    // Small round face.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.88F, p.R * 0.92F, skin, p.roll);
    drawBlush(cv, r, rgb(255, 148, 148, 0.6F));
    // Short boyish hair: a cap with little spikes and a side fringe.
    cv.fillEllipse(p.cx, p.cy - p.R * 0.56F, p.R * 0.92F, p.R * 0.56F, hair, p.roll);
    for (float x : {-0.68F, -0.34F, 0.0F, 0.34F, 0.68F}) {
        cv.fillPolygon({p.at(x - 0.16F, -0.68F), p.at(x + 0.16F, -0.68F),
                        p.at(x + (x < 0.0F ? -0.05F : 0.05F), -1.02F)},
                       hair);
    }
    cv.fillPolygon({p.at(-0.82F, -0.58F), p.at(-0.15F, -0.6F), p.at(-0.5F, -0.06F)},
                   hair);
    cv.fillPolygon({p.at(0.05F, -0.6F), p.at(0.6F, -0.58F), p.at(0.32F, -0.1F)},
                   hair);
    cv.fillEllipse(p.at(-0.28F, -0.7F).x, p.at(-0.28F, -0.7F).y, p.R * 0.08F,
                   p.R * 0.14F, hairHi, p.roll);
    // Big cheeky eyes.
    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(88, 118, 150), hair,
            0.23F, 0.22F, 1.0F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(88, 118, 150), hair,
            0.23F, 0.22F, 1.0F, false);
    drawBrowDots(cv, r, hair);
    // Nose dot.
    cv.fillEllipse(p.at(0.0F, 0.22F).x, p.at(0.0F, 0.22F).y, p.R * 0.04F,
                   p.R * 0.05F, skinShade, p.roll);
    // Cheeky grin.
    const float mw = p.R * (0.18F + 0.12F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.26F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.5F).x, p.at(0.0F, 0.5F).y, mw, mo,
                       rgb(150, 70, 76), p.roll);
        cv.fillEllipse(p.at(0.0F, 0.46F).x, p.at(0.0F, 0.46F).y, mw * 0.85F,
                       mo * 0.35F, rgb(255, 252, 250), p.roll);  // upper teeth
    } else {
        cv.strokeLine({p.at(-0.22F, 0.46F).x, p.at(-0.22F, 0.46F).y},
                      {p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y}, p.R * 0.04F,
                      rgb(150, 78, 76));
        cv.strokeLine({p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y},
                      {p.at(0.22F, 0.46F).x, p.at(0.22F, 0.46F).y}, p.R * 0.04F,
                      rgb(150, 78, 76));
    }
}

void drawGirl(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(253, 218, 196);
    const Rgba skinShade = rgb(236, 192, 166);
    const Rgba hair = rgb(96, 62, 46);
    const Rgba hairHi = rgb(132, 92, 66);
    const Rgba ribbon = rgb(244, 122, 150);

    // Pigtails (twin-tails): big soft puffs on each side, behind the head.
    cv.fillEllipse(p.at(-1.02F, 0.1F).x, p.at(-1.02F, 0.1F).y, p.R * 0.42F,
                   p.R * 0.56F, hair, p.roll);
    cv.fillEllipse(p.at(1.02F, 0.1F).x, p.at(1.02F, 0.1F).y, p.R * 0.42F,
                   p.R * 0.56F, hair, p.roll);
    cv.fillRoundedRect(p.cx - p.R * 0.9F, p.cy + p.R * 1.0F, p.cx + p.R * 0.9F,
                       p.cy + p.R * 2.2F, p.R * 0.5F, rgb(240, 170, 190));
    cv.fillRoundedRect(p.cx - p.R * 0.22F, p.cy + p.R * 0.6F, p.cx + p.R * 0.22F,
                       p.cy + p.R * 1.1F, p.R * 0.18F, skinShade);
    // Round face.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.86F, p.R * 0.9F, skin, p.roll);
    drawBlush(cv, r, rgb(255, 146, 152, 0.6F));
    // Hair crown + bangs.
    cv.fillEllipse(p.cx, p.cy - p.R * 0.5F, p.R * 0.94F, p.R * 0.62F, hair, p.roll);
    cv.fillPolygon({p.at(-0.9F, -0.5F), p.at(-0.3F, -0.55F), p.at(-0.6F, 0.02F)}, hair);
    cv.fillPolygon({p.at(-0.35F, -0.56F), p.at(0.35F, -0.56F), p.at(0.0F, 0.06F)}, hair);
    cv.fillPolygon({p.at(0.3F, -0.55F), p.at(0.9F, -0.5F), p.at(0.6F, 0.02F)}, hair);
    cv.fillEllipse(p.at(-0.34F, -0.62F).x, p.at(-0.34F, -0.62F).y, p.R * 0.08F,
                   p.R * 0.14F, hairHi, p.roll);
    // Ribbons on the pigtails.
    cv.fillPolygon({p.at(-1.02F, -0.28F), p.at(-1.3F, -0.44F), p.at(-1.3F, -0.1F)}, ribbon);
    cv.fillPolygon({p.at(-1.02F, -0.28F), p.at(-0.74F, -0.44F), p.at(-0.74F, -0.1F)}, ribbon);
    cv.fillPolygon({p.at(1.02F, -0.28F), p.at(1.3F, -0.44F), p.at(1.3F, -0.1F)}, ribbon);
    cv.fillPolygon({p.at(1.02F, -0.28F), p.at(0.74F, -0.44F), p.at(0.74F, -0.1F)}, ribbon);
    // Big sparkly eyes.
    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(150, 96, 176), hair,
            0.24F, 0.24F, 1.0F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(150, 96, 176), hair,
            0.24F, 0.24F, 1.0F, false);
    // Extra sparkle catchlights (only meaningful while the eyes are open).
    if (r.eyeOpenL > 0.35F) {
        const Pt s = p.at(-0.5F, -0.02F);
        cv.fillEllipse(s.x, s.y, p.R * 0.03F, p.R * 0.03F, rgb(255, 255, 255, 0.9F), p.roll);
    }
    if (r.eyeOpenR > 0.35F) {
        const Pt s = p.at(0.34F, -0.02F);
        cv.fillEllipse(s.x, s.y, p.R * 0.03F, p.R * 0.03F, rgb(255, 255, 255, 0.9F), p.roll);
    }
    drawBrowDots(cv, r, hair);
    // Nose + small smile.
    cv.fillEllipse(p.at(0.0F, 0.22F).x, p.at(0.0F, 0.22F).y, p.R * 0.035F,
                   p.R * 0.045F, skinShade, p.roll);
    const float mw = p.R * (0.14F + 0.12F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.24F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.5F).x, p.at(0.0F, 0.5F).y, mw, mo,
                       rgb(210, 96, 110), p.roll);
        cv.fillEllipse(p.at(0.0F, 0.52F).x, p.at(0.0F, 0.52F).y, mw * 0.6F,
                       mo * 0.5F, rgb(232, 128, 138), p.roll);
    } else {
        cv.strokeLine({p.at(-0.12F, 0.48F).x, p.at(-0.12F, 0.48F).y},
                      {p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y}, p.R * 0.032F,
                      rgb(206, 96, 110));
        cv.strokeLine({p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y},
                      {p.at(0.12F, 0.48F).x, p.at(0.12F, 0.48F).y}, p.R * 0.032F,
                      rgb(206, 96, 110));
    }
}

void drawTeen(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba skin = rgb(251, 219, 197);
    const Rgba skinShade = rgb(234, 194, 168);
    const Rgba hair = rgb(58, 52, 66);
    const Rgba hairHi = rgb(92, 84, 104);
    const Rgba collar = rgb(48, 66, 110);

    // Neat bob: hair rounded to the jaw, behind the head.
    cv.fillEllipse(p.cx, p.cy + p.R * 0.1F, p.R * 1.05F, p.R * 1.16F, hair, p.roll);
    // Blazer / sailor top.
    cv.fillRoundedRect(p.cx - p.R * 1.25F, p.cy + p.R * 1.05F, p.cx + p.R * 1.25F,
                       p.cy + p.R * 2.4F, p.R * 0.5F, rgb(236, 238, 244));
    // Sailor collar with stripes.
    cv.fillPolygon({p.at(-0.9F, 1.02F), p.at(0.9F, 1.02F), p.at(0.55F, 1.5F),
                    p.at(-0.55F, 1.5F)},
                   collar);
    cv.fillPolygon({p.at(-0.42F, 1.02F), p.at(0.42F, 1.02F), p.at(0.0F, 1.62F)},
                   rgb(236, 238, 244));
    cv.strokeLine({p.at(-0.8F, 1.12F).x, p.at(-0.8F, 1.12F).y},
                  {p.at(-0.5F, 1.44F).x, p.at(-0.5F, 1.44F).y}, p.R * 0.03F,
                  rgb(236, 238, 244));
    cv.strokeLine({p.at(0.8F, 1.12F).x, p.at(0.8F, 1.12F).y},
                  {p.at(0.5F, 1.44F).x, p.at(0.5F, 1.44F).y}, p.R * 0.03F,
                  rgb(236, 238, 244));
    // Neck.
    cv.fillRoundedRect(p.cx - p.R * 0.22F, p.cy + p.R * 0.6F, p.cx + p.R * 0.22F,
                       p.cy + p.R * 1.12F, p.R * 0.18F, skinShade);
    // Face.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.82F, p.R * 0.96F, skin, p.roll);
    cv.fillEllipse(p.at(0.0F, 0.46F).x, p.at(0.0F, 0.46F).y, p.R * 0.5F,
                   p.R * 0.42F, skin, p.roll);
    drawBlush(cv, r, rgb(255, 152, 156, 0.45F));
    // Front bob fringe framing the face.
    cv.fillEllipse(p.cx, p.cy - p.R * 0.56F, p.R * 0.92F, p.R * 0.54F, hair, p.roll);
    cv.fillPolygon({p.at(-0.95F, -0.5F), p.at(-0.9F, -0.9F), p.at(-0.62F, 0.5F),
                    p.at(-0.98F, 0.35F)},
                   hair);
    cv.fillPolygon({p.at(0.95F, -0.5F), p.at(0.9F, -0.9F), p.at(0.62F, 0.5F),
                    p.at(0.98F, 0.35F)},
                   hair);
    cv.fillPolygon({p.at(-0.7F, -0.6F), p.at(0.05F, -0.62F), p.at(-0.4F, 0.0F)}, hair);
    cv.fillPolygon({p.at(-0.05F, -0.62F), p.at(0.7F, -0.6F), p.at(0.35F, 0.0F)}, hair);
    cv.fillEllipse(p.at(-0.36F, -0.68F).x, p.at(-0.36F, -0.68F).y, p.R * 0.08F,
                   p.R * 0.16F, hairHi, p.roll);

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, skin, rgb(96, 74, 96), hair,
            0.21F, 0.2F, 1.0F, true);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, skin, rgb(96, 74, 96), hair,
            0.21F, 0.2F, 1.0F, true);
    // Nose + gentle smile.
    cv.fillEllipse(p.at(0.0F, 0.22F).x, p.at(0.0F, 0.22F).y, p.R * 0.04F,
                   p.R * 0.05F, skinShade, p.roll);
    const float mw = p.R * (0.14F + 0.13F * r.mouthWide);
    const float mo = p.R * (0.02F + 0.24F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.5F).x, p.at(0.0F, 0.5F).y, mw, mo,
                       rgb(196, 92, 104), p.roll);
        cv.fillEllipse(p.at(0.0F, 0.5F).x, p.at(0.0F, 0.5F).y, mw * 0.78F,
                       mo * 0.76F, rgb(120, 48, 60), p.roll);
    } else {
        cv.strokeLine({p.at(-0.12F, 0.5F).x, p.at(-0.12F, 0.5F).y},
                      {p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y}, p.R * 0.03F,
                      rgb(190, 96, 104));
        cv.strokeLine({p.at(0.0F, 0.54F).x, p.at(0.0F, 0.54F).y},
                      {p.at(0.12F, 0.5F).x, p.at(0.12F, 0.5F).y}, p.R * 0.03F,
                      rgb(190, 96, 104));
    }
}

void drawDog(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba fur = rgb(198, 150, 104);
    const Rgba furDk = rgb(166, 120, 78);
    const Rgba cream = rgb(246, 228, 202);
    const Rgba black = rgb(40, 34, 32);
    const Rgba tongue = rgb(232, 118, 138);

    cv.fillRoundedRect(p.cx - p.R * 1.0F, p.cy + p.R * 0.85F, p.cx + p.R * 1.0F,
                       p.cy + p.R * 2.2F, p.R * 0.6F, furDk);
    // Floppy droopy ears hanging from the top-sides (behind the head).
    cv.fillEllipse(p.at(-0.95F, 0.35F).x, p.at(-0.95F, 0.35F).y, p.R * 0.34F,
                   p.R * 0.66F, furDk, p.roll + 0.25F);
    cv.fillEllipse(p.at(0.95F, 0.35F).x, p.at(0.95F, 0.35F).y, p.R * 0.34F,
                   p.R * 0.66F, furDk, p.roll - 0.25F);
    // Round head.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.95F, p.R * 0.9F, fur, p.roll);
    // A patch over one eye for character.
    cv.fillEllipse(p.at(0.42F, -0.1F).x, p.at(0.42F, -0.1F).y, p.R * 0.34F,
                   p.R * 0.34F, furDk, p.roll);
    // Cream muzzle.
    cv.fillEllipse(p.at(0.0F, 0.34F).x, p.at(0.0F, 0.34F).y, p.R * 0.46F,
                   p.R * 0.4F, cream, p.roll);
    drawBlush(cv, r, rgb(255, 150, 140, 0.4F));

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, fur, rgb(78, 56, 44),
            rgb(120, 88, 60), 0.2F, 0.2F, 1.0F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, fur, rgb(78, 56, 44),
            rgb(120, 88, 60), 0.2F, 0.2F, 1.0F, false);
    drawBrowDots(cv, r, furDk);

    // Nose at the top of the muzzle.
    const Pt nose = p.at(0.0F, 0.22F);
    cv.fillEllipse(nose.x, nose.y, p.R * 0.12F, p.R * 0.1F, black, p.roll);
    cv.fillEllipse(nose.x - p.R * 0.03F, nose.y - p.R * 0.03F, p.R * 0.035F,
                   p.R * 0.03F, rgb(140, 140, 148, 0.8F), p.roll);
    // Happy mouth with a lolling tongue when open.
    const float mo = p.R * (0.02F + 0.32F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.5F).x, p.at(0.0F, 0.5F).y,
                       p.R * (0.2F + 0.08F * r.mouthWide), mo, rgb(120, 52, 60),
                       p.roll);
        cv.fillEllipse(p.at(0.0F, 0.56F).x, p.at(0.0F, 0.56F).y + mo * 0.4F,
                       p.R * 0.14F, mo * 0.7F, tongue, p.roll);
    } else {
        cv.strokeLine({nose.x, nose.y}, {p.at(0.0F, 0.44F).x, p.at(0.0F, 0.44F).y},
                      p.R * 0.026F, rgb(120, 88, 66));
        cv.strokeLine({p.at(0.0F, 0.44F).x, p.at(0.0F, 0.44F).y},
                      {p.at(-0.18F, 0.5F).x, p.at(-0.18F, 0.5F).y}, p.R * 0.026F,
                      rgb(120, 88, 66));
        cv.strokeLine({p.at(0.0F, 0.44F).x, p.at(0.0F, 0.44F).y},
                      {p.at(0.18F, 0.5F).x, p.at(0.18F, 0.5F).y}, p.R * 0.026F,
                      rgb(120, 88, 66));
    }
}

void drawBear(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba fur = rgb(152, 112, 82);
    const Rgba furDk = rgb(120, 86, 60);
    const Rgba muzzle = rgb(208, 178, 144);
    const Rgba black = rgb(38, 30, 28);

    cv.fillRoundedRect(p.cx - p.R * 1.0F, p.cy + p.R * 0.85F, p.cx + p.R * 1.0F,
                       p.cy + p.R * 2.2F, p.R * 0.6F, furDk);
    // Round ears on top.
    cv.fillEllipse(p.at(-0.66F, -0.86F).x, p.at(-0.66F, -0.86F).y, p.R * 0.32F,
                   p.R * 0.32F, fur, p.roll);
    cv.fillEllipse(p.at(0.66F, -0.86F).x, p.at(0.66F, -0.86F).y, p.R * 0.32F,
                   p.R * 0.32F, fur, p.roll);
    cv.fillEllipse(p.at(-0.66F, -0.84F).x, p.at(-0.66F, -0.84F).y, p.R * 0.16F,
                   p.R * 0.16F, muzzle, p.roll);
    cv.fillEllipse(p.at(0.66F, -0.84F).x, p.at(0.66F, -0.84F).y, p.R * 0.16F,
                   p.R * 0.16F, muzzle, p.roll);
    // Round head.
    cv.fillEllipse(p.cx, p.cy, p.R * 0.96F, p.R * 0.92F, fur, p.roll);
    // Muzzle.
    cv.fillEllipse(p.at(0.0F, 0.32F).x, p.at(0.0F, 0.32F).y, p.R * 0.44F,
                   p.R * 0.36F, muzzle, p.roll);
    drawBlush(cv, r, rgb(240, 150, 130, 0.45F));

    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, fur, rgb(60, 44, 36),
            rgb(90, 66, 46), 0.17F, 0.18F, 1.0F, false);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, fur, rgb(60, 44, 36),
            rgb(90, 66, 46), 0.17F, 0.18F, 1.0F, false);
    drawBrowDots(cv, r, furDk);

    // Nose + mouth.
    const Pt nose = p.at(0.0F, 0.18F);
    cv.fillEllipse(nose.x, nose.y, p.R * 0.13F, p.R * 0.1F, black, p.roll);
    const float mo = p.R * (0.02F + 0.28F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.44F).x, p.at(0.0F, 0.44F).y,
                       p.R * (0.14F + 0.08F * r.mouthWide), mo, rgb(110, 54, 58),
                       p.roll);
    } else {
        cv.strokeLine({nose.x, nose.y}, {p.at(0.0F, 0.4F).x, p.at(0.0F, 0.4F).y},
                      p.R * 0.028F, rgb(90, 64, 50));
        cv.strokeLine({p.at(0.0F, 0.4F).x, p.at(0.0F, 0.4F).y},
                      {p.at(-0.16F, 0.46F).x, p.at(-0.16F, 0.46F).y}, p.R * 0.028F,
                      rgb(90, 64, 50));
        cv.strokeLine({p.at(0.0F, 0.4F).x, p.at(0.0F, 0.4F).y},
                      {p.at(0.16F, 0.46F).x, p.at(0.16F, 0.46F).y}, p.R * 0.028F,
                      rgb(90, 64, 50));
    }
}

void drawAlien(Canvas& cv, const Rig& r) {
    const Pose& p = r.pose;
    const Rgba head = rgb(120, 198, 168);
    const Rgba headDk = rgb(92, 170, 142);
    const Rgba headHi = rgb(168, 224, 200);
    const Rgba rim = rgb(70, 148, 126);
    const Rgba eyeBlack = rgb(20, 24, 28);

    // Antennae (behind the head), lifting slightly with browUp.
    const float lift = (r.browUpL + r.browUpR) * 0.5F * 0.22F;
    for (float s : {-1.0F, 1.0F}) {
        const Pt base = p.at(s * 0.34F, -0.86F);
        const Pt tip = p.at(s * 0.52F, -1.5F - lift);
        cv.strokeLine({base.x, base.y}, {tip.x, tip.y}, p.R * 0.05F, headDk);
        cv.fillEllipse(tip.x, tip.y, p.R * 0.12F, p.R * 0.12F, rgb(150, 236, 120));
        cv.fillEllipse(tip.x - p.R * 0.03F, tip.y - p.R * 0.03F, p.R * 0.035F,
                       p.R * 0.035F, rgb(255, 255, 255, 0.85F));
    }
    // Slim neck / body.
    cv.fillRoundedRect(p.cx - p.R * 0.7F, p.cy + p.R * 0.95F, p.cx + p.R * 0.7F,
                       p.cy + p.R * 2.2F, p.R * 0.5F, headDk);
    // Smooth tapered head: rounded cranium + a pointed chin.
    cv.fillEllipse(p.cx, p.cy - p.R * 0.1F, p.R * 0.92F, p.R * 0.86F, head, p.roll);
    cv.fillPolygon({p.at(-0.58F, 0.3F), p.at(0.58F, 0.3F), p.at(0.0F, 1.08F)}, head);
    // Cranium sheen.
    cv.fillEllipse(p.at(-0.3F, -0.55F).x, p.at(-0.3F, -0.55F).y, p.R * 0.26F,
                   p.R * 0.18F, headHi, p.roll);
    // Big glossy black almond eyes: a teal rim, then the reused eye with a dark
    // sclera so blink + gaze + catchlight all still read.
    for (float s : {-1.0F, 1.0F}) {
        const Pt e = p.at(s * 0.42F, -0.10F);  // matches drawEye's eye line
        cv.fillEllipse(e.x, e.y, 0.3F * p.R * 1.14F, 0.24F * p.R * 1.35F * 1.14F,
                       rim, p.roll);
    }
    // Reuse drawEye with a dark sclera so blink + gaze + catchlight still read on
    // the big glossy black almond eyes.
    drawEye(cv, r, -1.0F, r.eyeOpenL, r.browUpL, head, eyeBlack, headDk, 0.3F,
            0.24F, 1.35F, false, eyeBlack);
    drawEye(cv, r, 1.0F, r.eyeOpenR, r.browUpR, head, eyeBlack, headDk, 0.3F,
            0.24F, 1.35F, false, eyeBlack);
    // Tiny mouth.
    const float mo = p.R * (0.015F + 0.12F * r.mouthOpen);
    if (r.mouthOpen > 0.12F) {
        cv.fillEllipse(p.at(0.0F, 0.62F).x, p.at(0.0F, 0.62F).y,
                       p.R * (0.06F + 0.05F * r.mouthWide), mo, rgb(60, 40, 50),
                       p.roll);
    } else {
        cv.strokeLine({p.at(-0.08F, 0.62F).x, p.at(-0.08F, 0.62F).y},
                      {p.at(0.08F, 0.62F).x, p.at(0.08F, 0.62F).y}, p.R * 0.03F,
                      headDk);
    }
}

struct Backdrop final {
    Rgba top, bottom;
};

Backdrop backdropFor(AvatarCharacterId id) {
    switch (id) {
        case AvatarCharacterId::Cat:
            return {rgb(206, 240, 224), rgb(150, 210, 196)};
        case AvatarCharacterId::Fox:
            return {rgb(255, 224, 196), rgb(250, 186, 150)};
        case AvatarCharacterId::ManMid:
            return {rgb(214, 224, 236), rgb(176, 192, 214)};
        case AvatarCharacterId::Woman:
            return {rgb(248, 220, 234), rgb(228, 184, 208)};
        case AvatarCharacterId::Boy:
            return {rgb(214, 240, 220), rgb(174, 216, 188)};
        case AvatarCharacterId::Girl:
            return {rgb(252, 224, 236), rgb(244, 190, 214)};
        case AvatarCharacterId::Teen:
            return {rgb(220, 228, 244), rgb(186, 200, 230)};
        case AvatarCharacterId::Dog:
            return {rgb(246, 232, 208), rgb(226, 198, 158)};
        case AvatarCharacterId::Bear:
            return {rgb(236, 222, 202), rgb(206, 182, 152)};
        case AvatarCharacterId::Alien:
            return {rgb(210, 234, 226), rgb(150, 200, 186)};
        case AvatarCharacterId::Human:
        default:
            return {rgb(226, 220, 248), rgb(196, 190, 236)};
    }
}

void drawCharacter(Canvas& cv, AvatarCharacterId id, const Rig& r) {
    switch (id) {
        case AvatarCharacterId::Cat:
            drawCat(cv, r);
            break;
        case AvatarCharacterId::Fox:
            drawFox(cv, r);
            break;
        case AvatarCharacterId::ManMid:
            drawManMid(cv, r);
            break;
        case AvatarCharacterId::Woman:
            drawWoman(cv, r);
            break;
        case AvatarCharacterId::Boy:
            drawBoy(cv, r);
            break;
        case AvatarCharacterId::Girl:
            drawGirl(cv, r);
            break;
        case AvatarCharacterId::Teen:
            drawTeen(cv, r);
            break;
        case AvatarCharacterId::Dog:
            drawDog(cv, r);
            break;
        case AvatarCharacterId::Bear:
            drawBear(cv, r);
            break;
        case AvatarCharacterId::Alien:
            drawAlien(cv, r);
            break;
        case AvatarCharacterId::Human:
        default:
            drawHuman(cv, r);
            break;
    }
}

}  // namespace

std::vector<AvatarCharacterInfo> avatarCharacterCatalog() {
    return {
        {AvatarCharacterId::Human, "human", "\xEC\x82\xAC\xEB\x9E\x8C"},          // 사람
        {AvatarCharacterId::Cat, "cat", "\xEA\xB3\xA0\xEC\x96\x91\xEC\x9D\xB4"},  // 고양이
        {AvatarCharacterId::Fox, "fox", "\xEC\x97\xAC\xEC\x9A\xB0"},              // 여우
        {AvatarCharacterId::ManMid, "man_mid",
         "\xEC\x95\x84\xEC\xA0\x80\xEC\x94\xA8"},  // 아저씨
        {AvatarCharacterId::Woman, "woman", "\xEC\x97\xAC\xEC\x9E\x90"},  // 여자
        {AvatarCharacterId::Boy, "boy",
         "\xEB\x82\xA8\xEC\x9E\x90\xEC\x95\xA0"},  // 남자애
        {AvatarCharacterId::Girl, "girl",
         "\xEC\x97\xAC\xEC\x9E\x90\xEC\x95\xA0"},  // 여자애
        {AvatarCharacterId::Teen, "teen",
         "\xEC\x86\x8C\xEB\x85\x80(\xEC\xA4\x91\xED\x95\x99\xEC\x83\x9D)"},  // 소녀(중학생)
        {AvatarCharacterId::Dog, "dog",
         "\xEA\xB0\x95\xEC\x95\x84\xEC\xA7\x80"},                       // 강아지
        {AvatarCharacterId::Bear, "bear", "\xEA\xB3\xB0"},              // 곰
        {AvatarCharacterId::Alien, "alien",
         "\xEC\x99\xB8\xEA\xB3\x84\xEC\x9D\xB8"},  // 외계인
    };
}

std::vector<AvatarParameterBinding> characterAvatarBindings() {
    // Identical channel mapping to the placeholder, so the same
    // AvatarParameterMapper drives either renderer.
    return placeholderAvatarBindings();
}

namespace {

// Normalised (posX, posY) head-centre presets for the four corners. Chosen so the
// smaller overlay avatar (with its legibility card and a little body) sits neatly
// inside the frame edge.
struct CornerPreset final {
    float nx, ny;
};

CornerPreset cornerPreset(AvatarCorner corner) {
    switch (corner) {
        case AvatarCorner::LeftTop:
            return {0.17F, 0.30F};
        case AvatarCorner::RightTop:
            return {0.83F, 0.30F};
        case AvatarCorner::LeftBottom:
            return {0.17F, 0.66F};
        case AvatarCorner::RightBottom:
        default:
            return {0.83F, 0.66F};
    }
}

constexpr float kFrontScale = 1.0F;
constexpr float kFrontPosX = 0.5F;
constexpr float kFrontPosY = 0.46F;
constexpr float kCornerScale = 0.55F;

}  // namespace

void CharacterAvatarRenderer::applyFrontPreset() noexcept {
    userScale_.store(kFrontScale, std::memory_order_relaxed);
    posX_.store(kFrontPosX, std::memory_order_relaxed);
    posY_.store(kFrontPosY, std::memory_order_relaxed);
}

void CharacterAvatarRenderer::applyCornerPreset(AvatarCorner corner) noexcept {
    const CornerPreset preset = cornerPreset(corner);
    userScale_.store(kCornerScale, std::memory_order_relaxed);
    posX_.store(preset.nx, std::memory_order_relaxed);
    posY_.store(preset.ny, std::memory_order_relaxed);
}

CharacterAvatarRenderer::CharacterAvatarRenderer(std::uint32_t width,
                                                 std::uint32_t height,
                                                 AvatarCharacterId character,
                                                 AvatarPlacement placement,
                                                 std::uint32_t supersample)
    : width_(width),
      height_(height),
      supersample_(std::clamp<std::uint32_t>(supersample, 1U, 4U)),
      character_(character),
      placementMode_(placement.mode),
      placementCorner_(placement.corner) {
    // Seed the free transform from the initial placement so the very first frame
    // already matches the requested preset.
    if (placement.mode == AvatarPlacementMode::Corner) {
        applyCornerPreset(placement.corner);
    } else {
        applyFrontPreset();
    }
}

core::Result<AvatarRenderFrame> CharacterAvatarRenderer::render(
    core::TimestampNs timestamp,
    std::span<const AvatarParameterValue> parameters) {
    if (width_ == 0U || height_ == 0U) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "character avatar frame dimensions are empty"};
    }

    using Names = PlaceholderAvatarParameterNames;
    Rig r;
    r.eyeOpenL = clampf(valueOf(parameters, Names::kEyeOpenLeft, 1.0F), 0.0F, 1.0F);
    r.eyeOpenR = clampf(valueOf(parameters, Names::kEyeOpenRight, 1.0F), 0.0F, 1.0F);
    r.browUpL = clampf(valueOf(parameters, Names::kBrowUpLeft, 0.0F), 0.0F, 1.0F);
    r.browUpR = clampf(valueOf(parameters, Names::kBrowUpRight, 0.0F), 0.0F, 1.0F);
    r.mouthOpen = clampf(valueOf(parameters, Names::kMouthOpen, 0.0F), 0.0F, 1.0F);
    r.mouthWide = clampf(valueOf(parameters, Names::kMouthWide, 0.0F), 0.0F, 1.0F);
    r.yaw = clampf(valueOf(parameters, Names::kHeadYaw, 0.0F), -1.0F, 1.0F);
    r.pitch = clampf(valueOf(parameters, Names::kHeadPitch, 0.0F), -1.0F, 1.0F);
    r.roll = clampf(valueOf(parameters, Names::kHeadRoll, 0.0F), -1.0F, 1.0F);
    r.gaze = r.yaw * 0.6F;

    const AvatarCharacterId id = character_.load(std::memory_order_relaxed);
    const AvatarPlacementMode mode = placementMode_.load(std::memory_order_relaxed);
    const float scale = userScale_.load(std::memory_order_relaxed);
    const float nx = posX_.load(std::memory_order_relaxed);
    const float ny = posY_.load(std::memory_order_relaxed);

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    Canvas cv{width_, height_, supersample_};

    // Free transform: the head radius is baseR * userScale, and the head centre is
    // the normalised (posX, posY) mapped into the frame, plus a small yaw/pitch
    // parallax. Clamp so at least part of the head always stays on-frame — the
    // avatar can be moved and resized freely but never fully lost. Placement mode
    // now controls only the backdrop style (opaque front vs transparent overlay).
    const float baseR = std::min(w, h) * 0.30F;
    Pose pose;
    pose.roll = r.roll * 0.42F;  // radians
    pose.R = baseR * scale;
    const float rawCx = nx * w + r.yaw * pose.R * 0.20F;
    const float rawCy = ny * h - r.pitch * pose.R * 0.16F;
    const float marginX = pose.R * 0.4F;
    const float marginY = pose.R * 0.4F;
    pose.cx = clampf(rawCx, marginX, w - marginX);
    pose.cy = clampf(rawCy, marginY, h - marginY);

    if (mode == AvatarPlacementMode::Front) {
        cv.fillVerticalGradient(backdropFor(id).top, backdropFor(id).bottom);
    } else {
        cv.clearTransparent();
        // Soft rounded backdrop card so the avatar stays legible over any screen.
        cv.fillRoundedRect(pose.cx - pose.R * 1.5F, pose.cy - pose.R * 1.7F,
                           pose.cx + pose.R * 1.5F, pose.cy + pose.R * 1.9F,
                           pose.R * 0.5F, rgb(20, 24, 34, 0.42F));
    }
    r.pose = pose;

    drawCharacter(cv, id, r);

    const auto stride = width_ * 4U;
    return AvatarRenderFrame::fromBgra(timestamp, width_, height_, stride,
                                       cv.toBgra());
}

}  // namespace creator::avatar
