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
             bool drawBrow) {
    const Pose& p = r.pose;
    const float ex = side * 0.42F;
    const float ey = -0.10F;
    const Pt e = p.at(ex, ey);
    const float rx = eyeW * p.R;
    const float ry = eyeH * p.R * almond;

    // Sclera.
    cv.fillEllipse(e.x, e.y, rx, ry, rgb(255, 253, 250), p.roll);
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

struct Backdrop final {
    Rgba top, bottom;
};

Backdrop backdropFor(AvatarCharacterId id) {
    switch (id) {
        case AvatarCharacterId::Cat:
            return {rgb(206, 240, 224), rgb(150, 210, 196)};
        case AvatarCharacterId::Fox:
            return {rgb(255, 224, 196), rgb(250, 186, 150)};
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
    };
}

std::vector<AvatarParameterBinding> characterAvatarBindings() {
    // Identical channel mapping to the placeholder, so the same
    // AvatarParameterMapper drives either renderer.
    return placeholderAvatarBindings();
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
      placementCorner_(placement.corner) {}

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
    const AvatarCorner corner = placementCorner_.load(std::memory_order_relaxed);

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);
    Canvas cv{width_, height_, supersample_};

    // Head geometry differs by placement.
    Pose pose;
    pose.roll = r.roll * 0.42F;  // radians
    if (mode == AvatarPlacementMode::Front) {
        cv.fillVerticalGradient(backdropFor(id).top, backdropFor(id).bottom);
        pose.R = std::min(w, h) * 0.30F;
        pose.cx = w * 0.5F + r.yaw * pose.R * 0.22F;
        pose.cy = h * 0.46F - r.pitch * pose.R * 0.16F;
    } else {
        cv.clearTransparent();
        pose.R = std::min(w, h) * 0.17F;
        const float mx = pose.R * 1.55F;  // margin from edges
        const bool left = corner == AvatarCorner::LeftBottom ||
                          corner == AvatarCorner::LeftTop;
        const bool top = corner == AvatarCorner::LeftTop ||
                         corner == AvatarCorner::RightTop;
        pose.cx = (left ? mx : w - mx) + r.yaw * pose.R * 0.2F;
        pose.cy = (top ? mx : h - mx * 1.15F) - r.pitch * pose.R * 0.14F;
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
