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
    add(1, 1, diag, 0.8f);          // shear
    add(1, -1, diag, 0.8f);
    add(0, 1, dx, 1.f);             // structural last: they win
    add(1, 0, dy, 1.f);
    // bending: every particle wants to sit halfway between its neighbours
    // along a row and along a column (zero curvature)
    for (int i = 0; i < rows; ++i)
        for (int j = 1; j + 1 < cols; ++j) bends_.push_back({idx(i, j - 1), idx(i, j), idx(i, j + 1)});
    for (int i = 1; i + 1 < rows; ++i)
        for (int j = 0; j < cols; ++j) bends_.push_back({idx(i - 1, j), idx(i, j), idx(i + 1, j)});

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
        pin_pos_ = pos[index];
        // rest distances for the anti-stretch tether (straight line on the
        // flat rug: a lower bound of the distance along the cloth)
        tether_.resize(count());
        float pu = uv[2 * index] * width, pv = uv[2 * index + 1] * height;
        for (int k = 0; k < count(); ++k)
            tether_[k] = std::hypot(uv[2 * k] * width - pu, uv[2 * k + 1] * height - pv);
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
    int n = std::max(1, substeps);
    for (int k = 0; k < n; ++k) substep(dt / n);
}

void Cloth::substep(float dt) {
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
    if (pin_index_ >= 0) {
        // the hand catches up with the cursor, but a heavy rug cannot be
        // yanked across the screen in one frame
        float k = 1 - std::exp(-drag_follow * dt);
        V3 d{(pin_target_.x - pin_pos_.x) * k, (pin_target_.y - pin_pos_.y) * k,
             (pin_target_.z - pin_pos_.z) * k};
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), lim = max_drag_speed * dt;
        if (len > lim) d = {d.x * lim / len, d.y * lim / len, d.z * lim / len};
        pin_pos_ = {pin_pos_.x + d.x, pin_pos_.y + d.y, pin_pos_.z + d.z};
        pos[pin_index_] = pin_pos_;
    }

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
        for (const Bend& e : bends_) {
            float wa = inv_mass_[e.a], wb = inv_mass_[e.b], wc = inv_mass_[e.c];
            float ws = wb + 0.25f * (wa + wc);
            if (ws <= 0) continue;
            V3& a = pos[e.a];
            V3& b = pos[e.b];
            V3& c = pos[e.c];
            float s = bend_stiffness / ws;
            float cx = (b.x - 0.5f * (a.x + c.x)) * s, cy = (b.y - 0.5f * (a.y + c.y)) * s,
                  cz = (b.z - 0.5f * (a.z + c.z)) * s;
            b.x -= wb * cx; b.y -= wb * cy; b.z -= wb * cz;
            a.x += 0.5f * wa * cx; a.y += 0.5f * wa * cy; a.z += 0.5f * wa * cz;
            c.x += 0.5f * wc * cx; c.y += 0.5f * wc * cy; c.z += 0.5f * wc * cz;
        }
        if (pin_index_ >= 0) {
            // long-range tethers: nothing can be further from the hand than
            // the cloth between them allows, so the rug never stretches
            const V3 h = pos[pin_index_];
            for (int k = 0; k < n; ++k) {
                V3& p = pos[k];
                float dx = p.x - h.x, dy = p.y - h.y, dz = p.z - h.z;
                float d2 = dx * dx + dy * dy + dz * dz, r = tether_[k] * 1.01f;
                if (d2 <= r * r || d2 < 1e-6f) continue;
                float s = r / std::sqrt(d2);
                p = {h.x + dx * s, h.y + dy * s, h.z + dz * s};
            }
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

void Cloth::sample(float u, float v, V3& p, V3& n) const {
    float fx = std::clamp(u / width, 0.f, 1.f) * (cols - 1);
    float fy = std::clamp(v / height, 0.f, 1.f) * (rows - 1);
    int j = std::min(int(fx), cols - 2), i = std::min(int(fy), rows - 2);
    float tx = fx - j, ty = fy - i;
    auto mix = [&](const std::vector<V3>& a) {
        const V3 &p00 = a[idx(i, j)], &p01 = a[idx(i, j + 1)], &p10 = a[idx(i + 1, j)],
                 &p11 = a[idx(i + 1, j + 1)];
        float w00 = (1 - tx) * (1 - ty), w01 = tx * (1 - ty), w10 = (1 - tx) * ty, w11 = tx * ty;
        return V3{p00.x * w00 + p01.x * w01 + p10.x * w10 + p11.x * w11,
                  p00.y * w00 + p01.y * w01 + p10.y * w10 + p11.y * w11,
                  p00.z * w00 + p01.z * w01 + p10.z * w10 + p11.z * w11};
    };
    p = mix(pos);
    n = mix(normals);
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    n = len > 1e-6f ? V3{n.x / len, n.y / len, n.z / len} : V3{0, 0, 1};
}

void Cloth::flat_target(std::vector<V3>& out) const {
    // 2D Procrustes: best rotation + translation mapping the flat rug onto
    // the particles that lie flat on the floor right now.
    int n = count();
    auto fit = [&](bool only_flat, double& cx, double& cy, double& rx, double& ry,
                   double& angle) {
        double sx = 0, sy = 0, su = 0, sv = 0, w = 0;
        for (int k = 0; k < n; ++k) {
            if (only_flat && (normals[k].z < 0.97f || pos[k].z > 1.f)) continue;
            sx += pos[k].x; sy += pos[k].y;
            su += uv[2 * k] * width; sv += uv[2 * k + 1] * height;
            w += 1;
        }
        if (w < 8) return false;
        cx = sx / w; cy = sy / w; rx = su / w; ry = sv / w;
        double a = 0, b = 0;
        for (int k = 0; k < n; ++k) {
            if (only_flat && (normals[k].z < 0.97f || pos[k].z > 1.f)) continue;
            double u = uv[2 * k] * width - rx, v = uv[2 * k + 1] * height - ry;
            double x = pos[k].x - cx, y = pos[k].y - cy;
            a += u * x + v * y;
            b += u * y - v * x;
        }
        angle = std::atan2(b, a);
        return true;
    };
    double cx = 0, cy = 0, rx = 0, ry = 0, angle = 0;
    if (!fit(true, cx, cy, rx, ry, angle)) fit(false, cx, cy, rx, ry, angle);
    double c = std::cos(angle), s = std::sin(angle);
    out.resize(n);
    for (int k = 0; k < n; ++k) {
        double u = uv[2 * k] * width - rx, v = uv[2 * k + 1] * height - ry;
        out[k] = {float(cx + c * u - s * v), float(cy + s * u + c * v), 0.f};
    }
}

float Cloth::deviation(const std::vector<V3>& target, int& worst) const {
    float best = 0;
    worst = -1;
    for (int k = 0; k < count(); ++k) {
        float d = std::sqrt((pos[k].x - target[k].x) * (pos[k].x - target[k].x) +
                            (pos[k].y - target[k].y) * (pos[k].y - target[k].y) +
                            pos[k].z * pos[k].z);
        if (d > best) {
            best = d;
            worst = k;
        }
    }
    return best;
}
