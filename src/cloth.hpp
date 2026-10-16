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
    float gravity = -4000.f;        // px/s^2: heavy, it drops and stays down
    float air_damping = 0.97f;      // wool in air: no fluttering
    float floor_friction = 0.55f;
    float static_speed = 0.35f;     // px/step: slower than this sticks
    int iterations = 12;
    int substeps = 2;               // smaller time steps: stiffer, steadier cloth
    // A heavy wool rug: resists bending (smooth, wide folds instead of
    // paper-like creases) and follows the hand with some inertia.
    float bend_stiffness = 0.3f;    // curvature constraint, per iteration
    float max_drag_speed = 900.f;   // px/s the grabbed point can move
    float drag_follow = 8.f;        // 1/s, how quickly it catches up

    std::vector<V3> pos, prev, normals;
    std::vector<float> uv;          // 2 per particle
    std::vector<uint16_t> triangles;

    int count() const { return cols * rows; }
    int idx(int i, int j) const { return i * cols + j; }

    void step(float dt);            // advances dt in `substeps` sub-steps
    void compute_normals();

    // Grab particle `index` and pull it towards `target` (smoothed, so the
    // rug feels heavy).  Call every frame with the new target.
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

    // Point and normal of the surface at rug coordinates (u, v), in px of
    // the flat rug (0..width, 0..height).
    void sample(float u, float v, V3& p, V3& n) const;
    // Where every particle would be if the rug lay flat right where it lies
    // now (fitted to the parts that are flat on the floor).
    void flat_target(std::vector<V3>& out) const;
    // Largest distance from that flat target; `worst` gets the particle.
    float deviation(const std::vector<V3>& target, int& worst) const;

private:
    void substep(float dt);
    struct Edge {
        int a, b;
        float rest, stiffness;
    };
    struct Bend {
        int a, b, c;                // b is between a and c
    };
    std::vector<Edge> edges_;
    std::vector<Bend> bends_;
    std::vector<float> tether_;     // rest distance to the grabbed particle
    V3 pin_pos_;                    // where the hand actually is now
    std::vector<float> inv_mass_, base_floor_;
    int pin_index_ = -1;
    V3 pin_target_;
    int calm_steps_ = 0;
};
