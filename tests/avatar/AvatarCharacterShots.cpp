// Visual proof harness for the built-in VTuber characters.
//
// It renders every CharacterAvatarRenderer character through the exact
// tracking -> map -> render chain the Studio avatar source uses, driven by
// sample expression sweeps (neutral / blink / mouth open / head turn), and
// writes PNGs so the LOOK is directly inspectable. It also renders a corner
// (picture-in-picture) placement over a synthetic "screen" to prove the
// transparent overlay composites, and a front-mode shot.
//
// Qt-free: links only cs_avatar and writes PNGs with a tiny embedded encoder.
//
// Usage: AvatarCharacterShots [outDir]   (default: avatar-shots)

#include "avatar/AvatarParameterMapper.h"
#include "avatar/CharacterAvatarRenderer.h"
#include "avatar/ExpressionParameters.h"
#include "core/Timebase.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace av = creator::avatar;

namespace {

// ---- Minimal PNG (RGBA8, stored deflate) -----------------------------------

std::uint32_t crc32of(const std::uint8_t* data, std::size_t len,
                      std::uint32_t crc = 0xFFFFFFFFU) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        init = true;
    }
    for (std::size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    return crc;
}

void putBE32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void writeChunk(std::vector<std::uint8_t>& out, const char* type,
                const std::vector<std::uint8_t>& data) {
    putBE32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t crc =
        crc32of(out.data() + start, out.size() - start) ^ 0xFFFFFFFFU;
    putBE32(out, crc);
}

// RGBA8 rows -> PNG file.
bool writePng(const std::filesystem::path& path, std::uint32_t w,
              std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    // Filtered raw data: each scanline gets a 0 filter byte.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (w * 4U + 1U));
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const std::size_t o = static_cast<std::size_t>(y) * w * 4U;
        raw.insert(raw.end(), rgba.begin() + o, rgba.begin() + o + w * 4U);
    }
    // zlib stream: header + stored deflate + adler32.
    std::vector<std::uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = (pos + n) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    // adler32
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    putBE32(z, (b << 16) | a);

    std::vector<std::uint8_t> out = {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<std::uint8_t> ihdr;
    putBE32(ihdr, w);
    putBE32(ihdr, h);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    writeChunk(out, "IHDR", ihdr);
    writeChunk(out, "IDAT", z);
    writeChunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return f.good();
}

// BGRA frame -> RGBA rows.
std::vector<std::uint8_t> bgraToRgba(const av::AvatarRenderFrame& frame) {
    const auto bytes = frame.bytes();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(frame.width()) *
                                   frame.height() * 4U);
    for (std::uint32_t y = 0; y < frame.height(); ++y) {
        for (std::uint32_t x = 0; x < frame.width(); ++x) {
            const auto i = static_cast<std::size_t>(y) * frame.stride() + x * 4U;
            const auto o = (static_cast<std::size_t>(y) * frame.width() + x) * 4U;
            rgba[o + 0] = bytes[i + 2];
            rgba[o + 1] = bytes[i + 1];
            rgba[o + 2] = bytes[i + 0];
            rgba[o + 3] = bytes[i + 3];
        }
    }
    return rgba;
}

// ---- Expression sweeps -----------------------------------------------------

av::ExpressionParameters neutral() {
    av::ExpressionParameters e;
    e.eyeOpenLeft = 1.0F;
    e.eyeOpenRight = 1.0F;
    return e;
}
av::ExpressionParameters blink() {
    auto e = neutral();
    e.eyeOpenLeft = 0.04F;
    e.eyeOpenRight = 0.04F;
    return e;
}
av::ExpressionParameters mouthOpen() {
    auto e = neutral();
    e.mouthOpen = 0.95F;
    e.mouthWide = 0.45F;
    e.browUpLeft = 0.6F;
    e.browUpRight = 0.6F;
    return e;
}
av::ExpressionParameters turn() {
    auto e = neutral();
    e.headYaw = 0.85F;
    e.headRoll = 0.35F;
    e.headPitch = -0.25F;
    e.browUpLeft = 0.4F;
    return e;
}

bool renderTo(const std::filesystem::path& path,
              const av::AvatarParameterMapper& mapper,
              av::CharacterAvatarRenderer& renderer,
              const av::ExpressionParameters& expr) {
    const auto mapped = mapper.map(expr);
    if (!mapped.hasValue()) return false;
    const auto frame = renderer.render(creator::core::TimestampNs{}, mapped.value());
    if (!frame.hasValue()) return false;
    return writePng(path, frame.value().width(), frame.value().height(),
                    bgraToRgba(frame.value()));
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "avatar-shots";
    std::filesystem::create_directories(outDir);

    auto mapperRes = av::AvatarParameterMapper::create(av::characterAvatarBindings());
    if (!mapperRes.hasValue()) {
        std::fprintf(stderr, "mapper failed\n");
        return 2;
    }
    const auto& mapper = mapperRes.value();

    struct C {
        av::AvatarCharacterId id;
        const char* name;
    };
    const C chars[] = {{av::AvatarCharacterId::Human, "human"},
                       {av::AvatarCharacterId::Cat, "cat"},
                       {av::AvatarCharacterId::Fox, "fox"},
                       {av::AvatarCharacterId::ManMid, "man_mid"},
                       {av::AvatarCharacterId::Woman, "woman"},
                       {av::AvatarCharacterId::Boy, "boy"},
                       {av::AvatarCharacterId::Girl, "girl"},
                       {av::AvatarCharacterId::Teen, "teen"},
                       {av::AvatarCharacterId::Dog, "dog"},
                       {av::AvatarCharacterId::Bear, "bear"},
                       {av::AvatarCharacterId::Alien, "alien"}};
    struct S {
        const char* name;
        av::ExpressionParameters expr;
    };
    const S sweeps[] = {{"neutral", neutral()},
                        {"blink", blink()},
                        {"mouth", mouthOpen()},
                        {"turn", turn()}};

    constexpr std::uint32_t kSize = 512;
    int ok = 0, total = 0;
    for (const auto& c : chars) {
        av::CharacterAvatarRenderer renderer{
            kSize, kSize, c.id, {av::AvatarPlacementMode::Front, {}}, 3};
        for (const auto& s : sweeps) {
            const auto path =
                outDir / (std::string(c.name) + "_" + s.name + ".png");
            ++total;
            if (renderTo(path, mapper, renderer, s.expr)) {
                ++ok;
                std::printf("wrote %s\n", path.string().c_str());
            } else {
                std::fprintf(stderr, "FAILED %s\n", path.string().c_str());
            }
        }
    }

    // Front-mode hero shot (human, talking).
    {
        av::CharacterAvatarRenderer renderer{
            720, 720, av::AvatarCharacterId::Human,
            {av::AvatarPlacementMode::Front, {}}, 3};
        auto e = mouthOpen();
        e.headYaw = 0.25F;
        e.headRoll = 0.12F;
        ++total;
        if (renderTo(outDir / "front_mode.png", mapper, renderer, e)) ++ok;
    }

    // Corner overlay over a synthetic screen: prove the transparent PiP frame
    // composites over a screen recording.
    {
        constexpr std::uint32_t W = 960, H = 540;
        av::CharacterAvatarRenderer renderer{
            W, H, av::AvatarCharacterId::Cat,
            {av::AvatarPlacementMode::Corner, av::AvatarCorner::RightBottom}, 3};
        auto e = neutral();
        e.mouthOpen = 0.5F;
        e.headYaw = -0.3F;
        const auto mapped = mapper.map(e);
        const auto frame = renderer.render(creator::core::TimestampNs{}, mapped.value());
        if (frame.hasValue()) {
            // Synthetic "screen": soft slide-like background with bars.
            std::vector<std::uint8_t> screen(static_cast<std::size_t>(W) * H * 4U);
            for (std::uint32_t y = 0; y < H; ++y) {
                for (std::uint32_t x = 0; x < W; ++x) {
                    const auto o = (static_cast<std::size_t>(y) * W + x) * 4U;
                    const float t = static_cast<float>(y) / H;
                    std::uint8_t base = static_cast<std::uint8_t>(28 + t * 30);
                    // A couple of "content" bands.
                    bool band = (y > 80 && y < 130 && x > 80 && x < 640) ||
                                (y > 170 && y < 200 && x > 80 && x < 520) ||
                                (y > 230 && y < 260 && x > 80 && x < 560);
                    screen[o + 0] = band ? 70 : base;
                    screen[o + 1] = band ? 110 : static_cast<std::uint8_t>(base + 8);
                    screen[o + 2] = band ? 150 : static_cast<std::uint8_t>(base + 18);
                    screen[o + 3] = 255;
                }
            }
            // Composite avatar (straight alpha "over") onto the screen.
            const auto av = bgraToRgba(frame.value());
            for (std::size_t p = 0; p < screen.size(); p += 4) {
                const float a = av[p + 3] / 255.0F;
                for (int k = 0; k < 3; ++k)
                    screen[p + k] = static_cast<std::uint8_t>(
                        av[p + k] * a + screen[p + k] * (1.0F - a) + 0.5F);
            }
            ++total;
            if (writePng(outDir / "corner_overlay_composite.png", W, H, screen))
                ++ok;
        }
    }

    // ---- Free-transform proofs (position + size) ---------------------------
    {
        // A small avatar parked in a transparent bottom-right corner.
        av::CharacterAvatarRenderer renderer{
            960, 540, av::AvatarCharacterId::Fox,
            {av::AvatarPlacementMode::Corner, av::AvatarCorner::RightBottom}, 3};
        renderer.setUserScale(0.5F);
        renderer.setPosition(0.82F, 0.68F);
        auto e = neutral();
        e.mouthOpen = 0.3F;
        ++total;
        if (renderTo(outDir / "transform_small_corner.png", mapper, renderer, e))
            ++ok;
    }
    {
        // A large avatar centred with the opaque front backdrop.
        av::CharacterAvatarRenderer renderer{
            720, 720, av::AvatarCharacterId::Woman,
            {av::AvatarPlacementMode::Front, {}}, 3};
        renderer.setUserScale(1.9F);
        renderer.setPosition(0.5F, 0.5F);
        ++total;
        if (renderTo(outDir / "transform_large_center.png", mapper, renderer,
                     mouthOpen()))
            ++ok;
    }
    {
        // The same free transform moved to the top-left (transparent overlay).
        av::CharacterAvatarRenderer renderer{
            960, 540, av::AvatarCharacterId::Girl,
            {av::AvatarPlacementMode::Corner, av::AvatarCorner::LeftTop}, 3};
        renderer.setUserScale(0.62F);
        renderer.setPosition(0.17F, 0.26F);
        ++total;
        if (renderTo(outDir / "transform_moved_topleft.png", mapper, renderer,
                     turn()))
            ++ok;
    }
    // A small, freely-repositioned avatar composited over a synthetic screen —
    // exactly the usable-overlay case the user cares about.
    {
        constexpr std::uint32_t W = 1280, H = 720;
        av::CharacterAvatarRenderer renderer{
            W, H, av::AvatarCharacterId::Bear,
            {av::AvatarPlacementMode::Corner, av::AvatarCorner::LeftBottom}, 3};
        renderer.setUserScale(0.5F);
        renderer.setPosition(0.15F, 0.72F);
        auto e = neutral();
        e.mouthOpen = 0.6F;
        e.headYaw = 0.2F;
        const auto mapped = mapper.map(e);
        const auto frame =
            renderer.render(creator::core::TimestampNs{}, mapped.value());
        if (frame.hasValue()) {
            std::vector<std::uint8_t> screen(static_cast<std::size_t>(W) * H * 4U);
            for (std::uint32_t y = 0; y < H; ++y) {
                for (std::uint32_t x = 0; x < W; ++x) {
                    const auto o = (static_cast<std::size_t>(y) * W + x) * 4U;
                    const float t = static_cast<float>(y) / H;
                    const std::uint8_t base =
                        static_cast<std::uint8_t>(30 + t * 34);
                    const bool band =
                        (y > 90 && y < 150 && x > 110 && x < 900) ||
                        (y > 210 && y < 250 && x > 110 && x < 760) ||
                        (y > 300 && y < 340 && x > 110 && x < 820);
                    screen[o + 0] = band ? 70 : base;
                    screen[o + 1] = band ? 120 : static_cast<std::uint8_t>(base + 8);
                    screen[o + 2] = band ? 170 : static_cast<std::uint8_t>(base + 18);
                    screen[o + 3] = 255;
                }
            }
            const auto avatarRgba = bgraToRgba(frame.value());
            for (std::size_t p = 0; p < screen.size(); p += 4) {
                const float a = avatarRgba[p + 3] / 255.0F;
                for (int k = 0; k < 3; ++k)
                    screen[p + k] = static_cast<std::uint8_t>(
                        avatarRgba[p + k] * a + screen[p + k] * (1.0F - a) + 0.5F);
            }
            ++total;
            if (writePng(outDir / "overlay_over_screen.png", W, H, screen)) ++ok;
        }
    }

    std::printf("\navatar shots: %d/%d written to %s\n", ok, total,
                std::filesystem::absolute(outDir).string().c_str());
    return ok == total ? 0 : 1;
}
