#include "cloth.hpp"

#include <algorithm>
#include <cmath>

Cloth::Cloth(float w, float h, int c, int r, float ox, float oy)
    : cols(c), rows(r), width(w), height(h) {
    int n = count();
    pos.resize(n);
    prev.resize(n);
    normals.assign(n, V3{0, 0, 1});
    inv_mass_.assign(n, 1.f);
    base_floor_.assign(n, 0.f);
    uv.resize(2 * n);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            uv[2 * idx(i, j)] = float(j) / (cols - 1);
            uv[2 * idx(i, j) + 1] = float(i) / (rows - 1);
        }
    flat_pose(ox, oy, 0, pos);
    prev = pos;

    float dx = w / (c - 1), dy = h / (r - 1), diag = std::hypot(dx, dy);
    auto add = [&](int di, int dj, float rest, float k) {
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                int i2 = i + di, j2 = j + dj;
                if (i2 >= 0 && i2 < rows && j2 >= 0 && j2 < cols)
                    edges_.push_back({idx(i, j), idx(i2, j2), rest, k});
            }
    };
    add(0, 2, 2 * dx, 0.25f);       // bending
    add(2, 0, 2 * dy, 0.25f);
    add(1, 1, diag, 0.6f);          // shear
    add(1, -1, diag, 0.6f);
    add(0, 1, dx, 1.f);             // structural last: they win
    add(1, 0, dy, 1.f);

    for (int i = 0; i + 1 < rows; ++i)
        for (int j = 0; j + 1 < cols; ++j) {
            uint16_t a = idx(i, j), b = a + 1, cc = a + cols, d = cc + 1;
            triangles.insert(triangles.end(), {a, cc, b, b, cc, d});
        }
}

void Cloth::flat_pose(float ox, float oy, float z, std::vector<V3>& out) const {
    out.resize(count());
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            out[idx(i, j)] = {ox + width * j / (cols - 1), oy + height * i / (rows - 1), z};
}

void Cloth::rolled_pose(float ox, float oy, float amount, std::vector<V3>& out) const {
    // The rolled part is an Archimedean spiral (layers one rug thickness
    // apart) sitting on the floor right where the flat part ends.
    flat_pose(ox, oy, 0, out);
    const float core = 9.f, t = std::max(thickness, 1.f);
    float bottom = oy + height;
    float unrolled = (1.f - std::clamp(amount, 0.f, 1.f)) * height;
    float rolled_len = height - unrolled;
    if (rolled_len <= 1e-3f) return;
    float r_out = std::sqrt(core * core + rolled_len * t / float(M_PI));
    float contact = bottom - unrolled;
    for (auto& p : out) {
        float s = bottom - p.y;               // distance from the bottom edge
        if (s <= unrolled) continue;
        float arc = s - unrolled;
        float disc = std::max(r_out * r_out - t * arc / float(M_PI), core * core);
        float phi = (r_out - std::sqrt(disc)) * 2 * float(M_PI) / t;
        float r = r_out - t * phi / (2 * float(M_PI));
        p.y = contact - r * std::sin(phi);
        p.z = r_out - r * std::cos(phi);
    }
}

void Cloth::set_pose(const std::vector<V3>& p) {
    pos = p;
    prev = p;
    compute_normals();
    calm_steps_ = 0;
}

void Cloth::compute_normals() {
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j) {
            const V3& l = pos[idx(i, std::max(j - 1, 0))];
            const V3& r = pos[idx(i, std::min(j + 1, cols - 1))];
            const V3& u = pos[idx(std::max(i - 1, 0), j)];
            const V3& d = pos[idx(std::min(i + 1, rows - 1), j)];
            V3 du{r.x - l.x, r.y - l.y, r.z - l.z};
            V3 dv{d.x - u.x, d.y - u.y, d.z - u.z};
            V3 n{du.y * dv.z - du.z * dv.y, du.z * dv.x - du.x * dv.z,
                 du.x * dv.y - du.y * dv.x};
            float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len < 1e-9f) n = {0, 0, 1};
            else n = {n.x / len, n.y / len, n.z / len};
            normals[idx(i, j)] = n;
        }
}

void Cloth::pin(int index, V3 target) {
    if (pin_index_ != index) {
        unpin();
        pin_index_ = index;
        inv_mass_[index] = 0;
    }
    pin_target_ = target;
    calm_steps_ = 0;
}

void Cloth::unpin() {
    if (pin_index_ >= 0) inv_mass_[pin_index_] = 1;
    pin_index_ = -1;
    calm_steps_ = 0;
}

void Cloth::set_obstacles(const std::vector<Rect>& rects, float h) {
    bool changed = false;
    for (int k = 0; k < count(); ++k) {
        float f = 0;
        for (auto& r : rects) {
            float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
            float rx = std::max((r.x1 - r.x0) / 2, 1.f), ry = std::max((r.y1 - r.y0) / 2, 1.f);
            float d = std::max(std::fabs(pos[k].x - cx) / rx, std::fabs(pos[k].y - cy) / ry);
            f = std::max(f, h * std::clamp(1.8f - d, 0.f, 1.f));
        }
        if (std::fabs(f - base_floor_[k]) > 0.01f) {
            base_floor_[k] = f;
            changed = true;
        }
    }
    if (changed) calm_steps_ = 0;
}

void Cloth::step(float dt) {
    int n = count();
    float g = gravity * dt * dt;
    for (int k = 0; k < n; ++k) {
        V3& p = pos[k];
        V3& q = prev[k];
        V3 v{(p.x - q.x) * air_damping, (p.y - q.y) * air_damping, (p.z - q.z) * air_damping};
        q = p;
        p.x += v.x;
        p.y += v.y;
        p.z += v.z + g;
    }
    if (pin_index_ >= 0) pos[pin_index_] = pin_target_;

    // Cheap self-collision: the turned-over part of the rug lies on top of
    // the rest, so its floor is one rug thickness higher.
    compute_normals();
    static thread_local std::vector<float> floor;
    floor.resize(n);
    for (int k = 0; k < n; ++k)
        floor[k] = base_floor_[k] + (normals[k].z < -0.2f ? thickness : 0.f);

    for (int it = 0; it < iterations; ++it) {
        for (const Edge& e : edges_) {
            float wa = inv_mass_[e.a], wb = inv_mass_[e.b], ws = wa + wb;
            if (ws <= 0) continue;
            V3& a = pos[e.a];
            V3& b = pos[e.b];
            float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 1e-6f) continue;
            float s = e.stiffness * (len - e.rest) / (len * ws);
            a.x += dx * s * wa; a.y += dy * s * wa; a.z += dz * s * wa;
            b.x -= dx * s * wb; b.y -= dy * s * wb; b.z -= dz * s * wb;
        }
        for (int k = 0; k < n; ++k)
            if (pos[k].z < floor[k]) pos[k].z = floor[k];
    }

    // Floor contact: no bouncing, strong friction, static sticking.
    float motion = 0;
    for (int k = 0; k < n; ++k) {
        V3& p = pos[k];
        V3& q = prev[k];
        if (p.z <= floor[k] + 0.5f) {
            q.z = p.z;
            float vx = p.x - q.x, vy = p.y - q.y;
            if (std::hypot(vx, vy) < static_speed) {
                vx = vy = 0;
            } else {
                vx *= 1 - floor_friction;
                vy *= 1 - floor_friction;
            }
            q.x = p.x - vx;
            q.y = p.y - vy;
        }
        motion = std::max({motion, std::fabs(p.x - q.x), std::fabs(p.y - q.y),
                           std::fabs(p.z - q.z)});
    }
    calm_steps_ = motion < 0.15f ? calm_steps_ + 1 : 0;
}
