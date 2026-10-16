// Unit tests for the physics and the JSON reader (no Wayland needed).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>

#include "../src/cloth.hpp"
#include "../src/json.hpp"

static int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static float max_z(const Cloth& c) {
    float z = 0;
    for (auto& p : c.pos) z = std::fmax(z, p.z);
    return z;
}

static void flat_rug_rests() {
    Cloth c(400, 250, 27, 17, 50, 50);
    auto start = c.pos;
    for (int k = 0; k < 80; ++k) c.step(1 / 60.f);
    float d = 0;
    for (int k = 0; k < c.count(); ++k)
        d = std::fmax(d, std::fabs(c.pos[k].x - start[k].x) + std::fabs(c.pos[k].y - start[k].y) +
                             std::fabs(c.pos[k].z - start[k].z));
    CHECK(d < 1e-3f);
    CHECK(c.sleeping());
}

static void corner_folds_over_and_stays() {
    Cloth c(400, 250, 27, 17, 50, 50);
    int corner = c.idx(0, c.cols - 1);
    for (int k = 0; k < 150; ++k) {
        float t = std::fmin(1.f, k / 90.f);
        c.pin(corner, {450.f - t * 315, 50.f + t * 180, 50.f});
        c.step(1 / 60.f);
    }
    c.unpin();
    for (int k = 0; k < 400 && !c.sleeping(); ++k) c.step(1 / 60.f);
    int flipped = 0;
    for (auto& n : c.normals) flipped += n.z < 0;
    CHECK(flipped > 10);
    CHECK(c.pos[corner].x < 300);
    float stretch = 0;
    for (int i = 0; i < c.rows; ++i)
        for (int j = 0; j + 1 < c.cols; ++j) {
            auto a = c.pos[c.idx(i, j)], b = c.pos[c.idx(i, j + 1)];
            float len = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                                  (a.z - b.z) * (a.z - b.z));
            stretch = std::fmax(stretch, len / (400.f / 26));
        }
    CHECK(stretch < 1.1f);
    CHECK(c.sleeping());
}

static void obstacles_make_bumps() {
    Cloth c(400, 250, 27, 17, 50, 50);
    c.set_obstacles({{230, 150, 270, 190}}, 10);
    for (int k = 0; k < 200; ++k) c.step(1 / 60.f);
    CHECK(max_z(c) > 7 && max_z(c) <= 11.f);   // a stiff rug arches a little over it
}

static void roll_poses() {
    Cloth c(800, 300, 54, 21, 100, 100);
    std::vector<V3> p;
    c.rolled_pose(100, 100, 0, p);
    CHECK(max_z(Cloth(c)) == 0);
    float ymin = 1e9, ymax = -1e9, zmax = 0;
    c.rolled_pose(100, 100, 1, p);
    for (auto& v : p) {
        ymin = std::fmin(ymin, v.y);
        ymax = std::fmax(ymax, v.y);
        zmax = std::fmax(zmax, v.z);
    }
    CHECK(ymax - ymin < 60);        // a compact roll at the bottom edge
    CHECK(zmax > 20 && zmax < 60);
    CHECK(ymin > 330);
}

// A heavy rug: a fast, rough drag must not stretch it like rubber or crumple
// it like paper - one clean fold, then it lies still.
static void heavy_rug_does_not_crumple() {
    Cloth c(900, 300, 61, 21, 350, 100);
    int corner = c.idx(0, 60);
    float z = 0, worst_stretch = 0;
    for (int f = 0; f < 52; ++f) {
        float t = std::fmin(1.f, (f + 1) / 12.f);       // 650 px in 0.2 s
        z = std::fmin(60, z + 6);
        c.pin(corner, {1250 - 650 * t, 100 + 280 * t, z});
        c.step(1 / 60.f);
        for (int i = 0; i < c.rows; ++i)
            for (int j = 0; j + 1 < c.cols; ++j) {
                auto a = c.pos[c.idx(i, j)], b = c.pos[c.idx(i, j + 1)];
                worst_stretch = std::fmax(worst_stretch,
                    std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z)) / 15.f);
            }
    }
    c.unpin();
    for (int k = 0; k < 600 && !c.sleeping(); ++k) c.step(1 / 60.f);
    int max_folds = 0;
    for (int i = 0; i < c.rows; ++i) {
        int state = 0, folds = 0;
        for (int j = 0; j < c.cols; ++j) {
            float nz = c.normals[c.idx(i, j)].z;
            int st = nz > 0.3f ? 1 : nz < -0.3f ? -1 : 0;
            if (st && state && st != state) ++folds;
            if (st) state = st;
        }
        max_folds = std::max(max_folds, folds);
    }
    std::printf("fast drag: max stretch %.2f, folds per row at rest %d, loop %.0f px\n",
                worst_stretch, max_folds, max_z(c));
    CHECK(worst_stretch < 1.15f);
    CHECK(max_folds == 1);
    CHECK(max_z(c) < 70);
    CHECK(c.sleeping());
}

// Left folded, the invisible hand lays the rug flat again, and the flat
// part stays where it was.
static void rug_straightens_itself() {
    Cloth c(900, 300, 61, 21, 350, 100);
    for (int f = 0; f < 90; ++f) {
        float t = std::fmin(1.f, (f + 1) / 40.f);
        c.pin(c.idx(0, 60), {1250 - 450 * t, 100 + 230 * t, 50});
        c.step(1 / 60.f);
    }
    c.unpin();
    for (int k = 0; k < 600 && !c.sleeping(); ++k) c.step(1 / 60.f);
    V3 still = c.pos[c.idx(20, 0)];          // bottom-left corner, never moved
    std::vector<V3> target;
    int worst;
    c.flat_target(target);
    CHECK(c.deviation(target, worst) > 200);
    V3 from = c.pos[worst], to = target[worst];
    for (int f = 0; f <= 200; ++f) {
        float e = std::fmin(1.f, f / 170.f);
        e = e * e * (3 - 2 * e);
        c.pin(worst, {from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e,
                      50 * std::sin(float(M_PI) * e)});
        c.step(1 / 60.f);
    }
    c.unpin();
    for (int k = 0; k < 600 && !c.sleeping(); ++k) c.step(1 / 60.f);
    c.flat_target(target);
    CHECK(c.deviation(target, worst) < 12);
    V3 now = c.pos[c.idx(20, 0)];
    CHECK(std::fabs(now.x - still.x) + std::fabs(now.y - still.y) < 15);
}

static void json_roundtrip() {
    bool ok = false;
    Json j = Json::parse(R"({"a":[1,2.5,"x\né"],"b":{"c":true,"d":null}})", &ok);
    CHECK(ok);
    CHECK(j["a"][1].number() == 2.5);
    CHECK(j["a"][2].string() == "x\n\xc3\xa9");
    CHECK(j["b"]["c"].boolean());
    Json k = Json::parse(j.dump(), &ok);
    CHECK(ok && k.dump() == j.dump());
    Json::parse("{\"a\":", &ok);
    CHECK(!ok);
}

static void speed() {
    Cloth c(900, 300, 61, 21, 0, 0);
    c.pin(c.idx(0, 60), {700, 100, 60});
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < 200; ++k) c.step(1 / 60.f);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count() / 200;
    std::printf("cloth step (61x21, %d iterations): %.3f ms\n", c.iterations, ms);
    CHECK(ms < 8);
}

int main() {
    flat_rug_rests();
    corner_folds_over_and_stays();
    obstacles_make_bumps();
    roll_poses();
    heavy_rug_does_not_crumple();
    rug_straightens_itself();
    json_roundtrip();
    speed();
    std::printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
