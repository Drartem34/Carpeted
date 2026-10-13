#include "rugtex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

struct C3 {
    float r, g, b;
};

const C3 RED{158, 28, 42}, NAVY{24, 34, 72}, DARK{14, 18, 38}, CREAM{236, 222, 190},
    TEAL{52, 128, 128}, GOLD{206, 150, 72}, ROSE{196, 82, 88}, SKY{92, 128, 176};

// Small deterministic noise in [-1, 1].
float hash_noise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFF) / 32767.5f - 1.f;
}

C3 knot(int i, int j, int H, int W) {
    const float fi = i, fj = j;
    const float edge = std::min({fi, fj, float(H - 1 - i), float(W - 1 - j)});
    C3 c = DARK;
    auto paint = [&](bool m, const C3& col) {
        if (m) c = col;
    };

    // ------------------------------------------------------------ borders
    paint(edge >= 1 && edge < 3, CREAM);
    paint(edge >= 1 && edge < 3 && (i + j) % 4 < 2, RED);
    paint(edge >= 3 && edge < 22, NAVY);
    float along = std::min(fi, float(H - 1 - i)) <= std::min(fj, float(W - 1 - j)) ? fj : fi;
    const float period = 18.f;
    float a = std::fmod(along, period) - period / 2, b = edge - 12.5f;
    bool band = edge >= 4 && edge < 21;
    paint(band && std::fabs(b - 4.f * std::sin(along * 2 * float(M_PI) / period)) < 0.9f, TEAL);
    float r = std::hypot(a, b), th = std::atan2(b, a);
    paint(band && r < 5.6f + 1.6f * std::cos(4 * th), RED);
    paint(band && r < 3.4f, CREAM);
    paint(band && r < 2.0f, ROSE);
    paint(band && r < 0.9f, NAVY);
    paint(band && std::fabs(std::fabs(a) - 7.5f) < 1.f && std::fabs(b) < 2.6f, GOLD);
    paint(edge >= 22 && edge < 23, CREAM);
    paint(edge >= 23 && edge < 25, RED);
    paint(edge >= 23 && edge < 25 && (i + j) % 6 == 0, CREAM);
    paint(edge >= 25 && edge < 26, NAVY);
    if (edge < 26) return c;

    // -------------------------------------------------------------- field
    const float cy = (H - 1) / 2.f, cx = (W - 1) / 2.f, fy = cy - 26, fx = cx - 26;
    const float ax = std::fabs((fj - cx) / fx), ay = std::fabs((fi - cy) / fy);
    c = RED;
    float f = std::cos(ax * 7) * std::cos(ay * 5) + 0.5f * std::cos((ax + ay) * 9);
    paint(std::fabs(f - 0.55f) < 0.06f, NAVY);
    paint(std::fabs(f + 0.75f) < 0.05f, CREAM);

    struct Floret {
        float x, y;
        C3 col;
    };
    static const Floret florets[] = {{0.35f, 0.62f, CREAM}, {0.62f, 0.3f, GOLD},
                                     {0.75f, 0.72f, SKY},   {0.2f, 0.88f, TEAL},
                                     {0.9f, 0.05f, CREAM},  {0.5f, 0.f, GOLD},
                                     {0.f, 0.85f, CREAM},   {0.48f, 0.85f, ROSE}};
    for (auto& fl : florets) {
        float d = std::fabs((ax - fl.x) * fx) + std::fabs((ay - fl.y) * fy);
        paint(d < 3.2f, fl.col);
        paint(d < 1.4f, NAVY);
    }

    float corner = std::pow((ax - 1) / 0.5f, 2.f) + std::pow((ay - 1) / 0.62f, 2.f);
    paint(corner < 1.f, TEAL);
    paint(corner < 0.8f, NAVY);
    paint(corner < 0.75f && std::fabs(std::cos(ax * 30) * std::cos(ay * 22)) < 0.12f, CREAM);
    paint(corner < 0.8f && corner > 0.7f, GOLD);

    // central medallion
    float mx = fj - cx, my = (fi - cy) * 1.15f;
    float mr = std::hypot(mx, my), mt = std::atan2(my, mx);
    float pend = std::exp(-std::pow(std::fabs(mx) - 52, 2.f) / 30.f) * (std::fabs(my) < 6);
    float outline = 34 * (1 + 0.16f * std::cos(8 * mt)) + 18 * std::pow(std::fabs(std::cos(mt)), 8.f);
    paint(mr < outline + 2 || pend > 0.3f, CREAM);
    paint(mr < outline || pend > 0.55f, NAVY);
    paint(std::fabs(mr - outline * 0.72f) < 1.f, TEAL);
    paint(mr < outline * 0.65f && std::fabs(std::cos(mt * 12 + mr * 0.35f)) < 0.18f, SKY);
    float star = 15 * (0.55f + 0.45f * std::pow(std::fabs(std::cos(4 * mt)), 3.f));
    paint(mr < star + 1.5f, CREAM);
    paint(mr < star, RED);
    paint(mr < 6.5f, CREAM);
    paint(mr < 4.5f, TEAL);
    paint(mr < 2.f, NAVY);
    return c;
}

}  // namespace

std::vector<uint8_t> generate_rug(int& width, int& height, int kw, int kh, int kp) {
    width = kw * kp;
    height = kh * kp;
    std::vector<uint8_t> out(size_t(width) * height * 4);
    for (int i = 0; i < kh; ++i)
        for (int j = 0; j < kw; ++j) {
            C3 c = knot(i, j, kh, kw);
            float wool = 1.f + 0.045f * hash_noise(j, i, 1);
            for (int y = 0; y < kp; ++y)
                for (int x = 0; x < kp; ++x) {
                    float u = (x + 0.5f) / kp - 0.5f, v = (y + 0.5f) / kp - 0.5f;
                    float bump = 1.06f - 0.35f * (u * u + v * v);
                    int px = j * kp + x, py = i * kp + y;
                    float s = wool * bump * (1.f + 0.04f * hash_noise(px, py, 2));
                    uint8_t* o = &out[(size_t(py) * width + px) * 4];
                    o[0] = uint8_t(std::clamp(c.r * s, 0.f, 255.f));
                    o[1] = uint8_t(std::clamp(c.g * s, 0.f, 255.f));
                    o[2] = uint8_t(std::clamp(c.b * s, 0.f, 255.f));
                    o[3] = 255;
                }
        }
    return out;
}
