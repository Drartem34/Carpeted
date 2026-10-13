"""Position-based (Verlet) cloth simulation for a heavy rug lying on a floor.

Coordinates are in logical screen pixels: x to the right, y down, z up
(height above the floor).  The rug is a regular grid of particles connected
by distance constraints:

* structural  - direct neighbours, keeps the rug from stretching
* shear       - diagonals, keeps cells from collapsing
* bending     - every second particle, makes the rug stiff but foldable

Constraints are split into groups whose edges share no particles, so every
group can be solved with vectorised numpy operations while still behaving
like Gauss-Seidel (which converges much faster than Jacobi).
"""

import numpy as np


def _slice_pairs(rows, cols, di, dj, axis, stride):
    """Split all edges (i, j) -> (i + di, j + dj) into groups without shared
    particles.  Every group is a pair of strided 2D slices, so it can be
    updated in place through numpy views without any fancy indexing."""
    lo = [max(0, -di), max(0, -dj)]
    hi = [rows - max(0, di), cols - max(0, dj)]
    pairs = []
    for residue in range(stride):
        start = list(lo)
        start[axis] = lo[axis] + (residue - lo[axis]) % stride
        if start[axis] >= hi[axis]:
            continue
        steps = [1, 1]
        steps[axis] = stride
        a = (slice(start[0], hi[0], steps[0]), slice(start[1], hi[1], steps[1]))
        b = (slice(start[0] + di, hi[0] + di, steps[0]),
             slice(start[1] + dj, hi[1] + dj, steps[1]))
        pairs.append((a, b))
    return pairs


class Cloth:
    def __init__(self, width, height, cols, rows, origin=(0.0, 0.0),
                 thickness=3.0):
        self.cols, self.rows = cols, rows
        self.width, self.height = float(width), float(height)
        self.thickness = float(thickness)
        self.count = cols * rows

        self.gravity = -2400.0          # px/s^2
        self.air_damping = 0.992
        self.floor_friction = 0.55      # share of sliding velocity lost per step
        self.static_speed = 0.35        # px/step below which a resting particle sticks
        self.iterations = 12

        self.dx = self.width / (cols - 1)
        self.dy = self.height / (rows - 1)

        self.pos = np.zeros((self.count, 3))
        self.reset_flat(origin)
        self.prev = self.pos.copy()

        jj, ii = np.meshgrid(np.arange(cols), np.arange(rows))
        self.uv = np.stack([jj.ravel() / (cols - 1),
                            ii.ravel() / (rows - 1)], axis=1).astype(np.float32)

        self.inv_mass = np.ones(self.count)
        self.pins = {}                  # particle index -> target xyz
        self._weights = None
        self.base_floor = np.zeros(self.count)   # bumps from things under the rug
        self.normals = np.zeros((self.count, 3))
        self.normals[:, 2] = 1.0
        self._build_constraints()
        self._build_triangles()
        self.calm_steps = 0

    # ------------------------------------------------------------------ setup
    def idx(self, i, j):
        return i * self.cols + j

    def reset_flat(self, origin, z=0.0):
        cols, rows = self.cols, self.rows
        xs = np.linspace(0.0, self.width, cols) + origin[0]
        ys = np.linspace(0.0, self.height, rows) + origin[1]
        X, Y = np.meshgrid(xs, ys)
        self.pos[:, 0] = X.ravel()
        self.pos[:, 1] = Y.ravel()
        self.pos[:, 2] = z
        if hasattr(self, "prev"):
            self.prev[:] = self.pos
        self.calm_steps = 0

    def _build_constraints(self):
        R, C = self.rows, self.cols
        dx, dy = self.dx, self.dy
        diag = float(np.hypot(dx, dy))
        kinds = [
            # (di, dj, axis to split along, stride, rest length, stiffness)
            (0, 2, 1, 4, 2 * dx, 0.25),     # bending
            (2, 0, 0, 4, 2 * dy, 0.25),
            (1, 1, 0, 2, diag, 0.6),        # shear
            (1, -1, 0, 2, diag, 0.6),
            (0, 1, 1, 2, dx, 1.0),          # structural, solved last
            (1, 0, 0, 2, dy, 1.0),
        ]
        self.groups = []
        for di, dj, axis, stride, rest, stiffness in kinds:
            for a, b in _slice_pairs(R, C, di, dj, axis, stride):
                self.groups.append((a, b, rest, stiffness))

    def _build_triangles(self):
        R, C = self.rows, self.cols
        tris = []
        for i in range(R - 1):
            for j in range(C - 1):
                a = self.idx(i, j)
                b, c, d = a + 1, a + C, a + C + 1
                tris.append((a, c, b))
                tris.append((b, c, d))
        self.triangles = np.array(tris, dtype=np.uint32)

    # ---------------------------------------------------------------- queries
    def compute_normals(self):
        P = self.pos.reshape(self.rows, self.cols, 3)
        du = np.gradient(P, axis=1)
        dv = np.gradient(P, axis=0)
        n = np.cross(du, dv)
        length = np.linalg.norm(n, axis=2, keepdims=True)
        n /= np.maximum(length, 1e-9)
        self.normals = n.reshape(-1, 3)
        return self.normals

    def set_obstacles(self, rects, height=14.0):
        """Things lying on the floor (x0, y0, x1, y1) make bumps in the rug."""
        floor = np.zeros(self.count)
        x, y = self.pos[:, 0], self.pos[:, 1]
        for x0, y0, x1, y1 in rects:
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            rx, ry = max((x1 - x0) / 2, 1.0), max((y1 - y0) / 2, 1.0)
            d = np.maximum(np.abs(x - cx) / rx, np.abs(y - cy) / ry)
            floor = np.maximum(floor, height * np.clip(1.8 - d, 0.0, 1.0))
        if not np.array_equal(floor, self.base_floor):
            self.base_floor = floor
            self.wake()

    @property
    def sleeping(self):
        return not self.pins and self.calm_steps > 45

    def wake(self):
        self.calm_steps = 0

    def centroid(self):
        return self.pos.mean(axis=0)

    # ------------------------------------------------------------- simulation
    def _group_weights(self):
        """Per-group correction factors (stiffness * mass share), cached until
        the set of pinned particles changes."""
        if self._weights is None:
            w = self.inv_mass.reshape(self.rows, self.cols, 1)
            self._weights = []
            for sa, sb, _, stiffness in self.groups:
                wa, wb = w[sa], w[sb]
                if (wa == 1.0).all() and (wb == 1.0).all():
                    self._weights.append((0.5 * stiffness, None))
                else:
                    wsum = np.maximum(wa + wb, 1e-9)
                    self._weights.append((stiffness * wa / wsum,
                                          stiffness * wb / wsum))
        return self._weights

    def pin(self, index, target):
        if index not in self.pins:
            self._weights = None
        self.pins[index] = np.asarray(target, dtype=float)
        self.inv_mass[index] = 0.0
        self.wake()

    def unpin_all(self):
        for index in self.pins:
            self.inv_mass[index] = 1.0
        self.pins.clear()
        self._weights = None
        self.wake()

    def step(self, dt):
        p, q = self.pos, self.prev
        velocity = (p - q) * self.air_damping
        q[:] = p
        p += velocity
        p[:, 2] += self.gravity * dt * dt

        for index, target in self.pins.items():
            p[index] = target

        # Cheap self-collision: the part of the rug that is turned over lies
        # on top of the rest of the rug, so its "floor" is one rug thickness
        # higher.  Good enough for folding a corner back.
        n = self.compute_normals()
        floor = self.base_floor + np.where(n[:, 2] < -0.2, self.thickness, 0.0)

        grid = p.reshape(self.rows, self.cols, 3)
        z = p[:, 2]
        weights = self._group_weights()
        for _ in range(self.iterations):
            for (sa, sb, rest, _), (ka, kb) in zip(self.groups, weights):
                pa, pb = grid[sa], grid[sb]          # views, updated in place
                d = pb - pa
                length = np.sqrt(np.einsum("ijk,ijk->ij", d, d))
                np.maximum(length, 1e-9, out=length)
                d *= (1.0 - rest / length)[..., None]
                if kb is None:                       # no pinned particles
                    d *= ka
                    pa += d
                    pb -= d
                else:
                    pa += d * ka
                    pb -= d * kb
            np.maximum(z, floor, out=z)

        # Floor contact: no bouncing, strong friction, static sticking.
        contact = p[:, 2] <= floor + 0.5
        if contact.any():
            q[contact, 2] = p[contact, 2]
            vxy = p[contact, :2] - q[contact, :2]
            speed = np.linalg.norm(vxy, axis=1, keepdims=True)
            vxy *= 1.0 - self.floor_friction
            vxy[speed[:, 0] < self.static_speed] = 0.0
            q[contact, :2] = p[contact, :2] - vxy

        motion = np.abs(p - q).max()
        self.calm_steps = self.calm_steps + 1 if motion < 0.15 else 0

    # ----------------------------------------------------------- persistence
    def state(self):
        return {"cols": self.cols, "rows": self.rows,
                "width": self.width, "height": self.height,
                "pos": np.round(self.pos, 2).tolist()}

    def load_state(self, state):
        try:
            if (state["cols"], state["rows"]) != (self.cols, self.rows):
                return False
            if abs(state["width"] - self.width) > 0.5 or \
                    abs(state["height"] - self.height) > 0.5:
                return False
            pos = np.asarray(state["pos"], dtype=float)
            if pos.shape != self.pos.shape or not np.isfinite(pos).all():
                return False
        except (KeyError, TypeError, ValueError):
            return False
        self.pos[:] = pos
        self.prev[:] = pos
        self.wake()
        return True
