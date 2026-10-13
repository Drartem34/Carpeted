// Position-based (Verlet) cloth: a heavy rug lying on a floor.
//
// Units are logical pixels: x right, y down, z up (height above the floor).
// Particles on a grid are joined by distance constraints: structural
// (neighbours), shear (diagonals) and bending (every second particle), which
// makes the rug stiff but foldable.  Constraints are solved Gauss-Seidel.
#pragma once
#include <cstdint>
#include <vector>

struct V3 {
    float x = 0, y = 0, z = 0;
};

struct Rect {
    float x0, y0, x1, y1;
};

class Cloth {
public:
    Cloth(float width, float height, int cols, int rows, float ox, float oy);

    int cols, rows;
    float width, height;
    float thickness = 3.5f;
    float gravity = -2400.f;        // px/s^2
    float air_damping = 0.992f;
    float floor_friction = 0.55f;
    float static_speed = 0.35f;     // px/step: slower than this sticks
    int iterations = 12;

    std::vector<V3> pos, prev, normals;
    std::vector<float> uv;          // 2 per particle
    std::vector<uint16_t> triangles;

    int count() const { return cols * rows; }
    int idx(int i, int j) const { return i * cols + j; }

    void step(float dt);
    void compute_normals();

    void pin(int index, V3 target);
    void unpin();
    int pinned() const { return pin_index_; }

    void set_obstacles(const std::vector<Rect>& rects, float height);
    bool sleeping() const { return pin_index_ < 0 && calm_steps_ > 45; }
    void wake() { calm_steps_ = 0; }

    // Poses for the roll-out animation.  amount = 0: flat, 1: fully rolled
    // carpet lying at the bottom edge.
    void flat_pose(float ox, float oy, float z, std::vector<V3>& out) const;
    void rolled_pose(float ox, float oy, float amount, std::vector<V3>& out) const;
    void set_pose(const std::vector<V3>& p);

private:
    struct Edge {
        int a, b;
        float rest, stiffness;
    };
    std::vector<Edge> edges_;
    std::vector<float> inv_mass_, base_floor_;
    int pin_index_ = -1;
    V3 pin_target_;
    int calm_steps_ = 0;
};
