"""Procedural textures: a Persian-style rug and a herringbone parquet floor.

All functions return straight (non-premultiplied) RGBA uint8 numpy arrays of
shape (height, width, 4).  The rug is generated knot by knot and then every
knot is blown up to a small block of pixels, which gives a woven look.
"""

import math

import numpy as np

PALETTE = {
    "red": (158, 28, 42),
    "deep_red": (118, 18, 32),
    "navy": (24, 34, 72),
    "dark": (14, 18, 38),
    "cream": (236, 222, 190),
    "teal": (52, 128, 128),
    "gold": (206, 150, 72),
    "rose": (196, 82, 88),
    "sky": (92, 128, 176),
}


def _paint(img, mask, color):
    img[mask] = PALETTE[color]


def generate_rug(knots_w=240, knots_h=150, knot_px=4, seed=7):
    rng = np.random.default_rng(seed)
    H, W = knots_h, knots_w
    img = np.zeros((H, W, 3), dtype=np.float32)
    ii, jj = np.mgrid[0:H, 0:W].astype(np.float32)
    edge = np.minimum.reduce([ii, jj, H - 1 - ii, W - 1 - jj])

    # ---------------------------------------------------------------- borders
    _paint(img, edge >= 0, "dark")
    _paint(img, (edge >= 1) & (edge < 3), "cream")
    checks = ((ii + jj) % 4 < 2) & (edge >= 1) & (edge < 3)
    _paint(img, checks, "red")
    _paint(img, (edge >= 3) & (edge < 22), "navy")

    # Coordinate running along the main border band and across it.
    along = np.where(np.minimum(ii, H - 1 - ii) <= np.minimum(jj, W - 1 - jj),
                     jj, ii)
    across = edge - 12.5
    period = 18.0
    a = (along % period) - period / 2
    b = across
    band = (edge >= 4) & (edge < 21)
    vine = band & (np.abs(b - 4.0 * np.sin(along * 2 * math.pi / period)) < 0.9)
    _paint(img, vine, "teal")
    r = np.hypot(a, b)
    theta = np.arctan2(b, a)
    petals = band & (r < 5.6 + 1.6 * np.cos(4 * theta))
    _paint(img, petals, "red")
    _paint(img, band & (r < 3.4), "cream")
    _paint(img, band & (r < 2.0), "rose")
    _paint(img, band & (r < 0.9), "navy")
    leaf = band & (np.abs(np.abs(a) - 7.5) < 1.0) & (np.abs(b) < 2.6)
    _paint(img, leaf, "gold")

    _paint(img, (edge >= 22) & (edge < 23), "cream")
    _paint(img, (edge >= 23) & (edge < 25), "red")
    dots = (edge >= 23) & (edge < 25) & ((ii + jj) % 6 == 0)
    _paint(img, dots, "cream")
    _paint(img, (edge >= 25) & (edge < 26), "navy")

    # ------------------------------------------------------------------ field
    field = edge >= 26
    cy, cx = (H - 1) / 2, (W - 1) / 2
    fy, fx = cy - 26, cx - 26
    y, x = (ii - cy) / fy, (jj - cx) / fx
    ax, ay = np.abs(x), np.abs(y)
    _paint(img, field, "red")

    # Arabesque vines over the field: contour lines of a symmetric function.
    f = np.cos(ax * 7.0) * np.cos(ay * 5.0) + 0.5 * np.cos((ax + ay) * 9.0)
    _paint(img, field & (np.abs(f - 0.55) < 0.06), "navy")
    _paint(img, field & (np.abs(f + 0.75) < 0.05), "cream")

    # Small florets scattered symmetrically.
    for px, py, color in [(0.35, 0.62, "cream"), (0.62, 0.3, "gold"),
                          (0.75, 0.72, "sky"), (0.2, 0.88, "teal"),
                          (0.9, 0.05, "cream"), (0.5, 0.0, "gold"),
                          (0.0, 0.85, "cream"), (0.48, 0.85, "rose")]:
        dxk = (ax - px) * fx
        dyk = (ay - py) * fy
        d = np.abs(dxk) + np.abs(dyk)
        _paint(img, field & (d < 3.2), color)
        _paint(img, field & (d < 1.4), "navy")

    # Corner spandrels.
    corner = ((ax - 1) / 0.5) ** 2 + ((ay - 1) / 0.62) ** 2
    _paint(img, field & (corner < 1.0), "teal")
    _paint(img, field & (corner < 0.8), "navy")
    cvine = np.cos(ax * 30) * np.cos(ay * 22)
    _paint(img, field & (corner < 0.75) & (np.abs(cvine) < 0.12), "cream")
    _paint(img, field & (corner < 0.8) & (corner > 0.7), "gold")

    # Central medallion.
    mx = (jj - cx)
    my = (ii - cy) * 1.15
    mr = np.hypot(mx, my)
    mt = np.arctan2(my, mx)
    pend = np.exp(-((np.abs(mx) - 52) ** 2) / 30.0) * (np.abs(my) < 6)
    outline = 34 * (1 + 0.16 * np.cos(8 * mt)) + 18 * np.abs(np.cos(mt)) ** 8
    _paint(img, field & ((mr < outline + 2.0) | (pend > 0.3)), "cream")
    _paint(img, field & ((mr < outline) | (pend > 0.55)), "navy")
    ring = field & (np.abs(mr - outline * 0.72) < 1.0)
    _paint(img, ring, "teal")
    swirl = np.cos(mt * 12 + mr * 0.35)
    _paint(img, field & (mr < outline * 0.65) & (np.abs(swirl) < 0.18), "sky")
    star = 15 * (0.55 + 0.45 * np.abs(np.cos(4 * mt)) ** 3)
    _paint(img, field & (mr < star + 1.5), "cream")
    _paint(img, field & (mr < star), "red")
    _paint(img, field & (mr < 6.5), "cream")
    _paint(img, field & (mr < 4.5), "teal")
    _paint(img, field & (mr < 2.0), "navy")

    # ------------------------------------------------- wool & knot structure
    img *= rng.normal(1.0, 0.045, size=(H, W, 1)).astype(np.float32)
    big = np.repeat(np.repeat(img, knot_px, axis=0), knot_px, axis=1)
    k = (np.arange(knot_px) + 0.5) / knot_px - 0.5
    bump = 1.06 - 0.35 * (k[:, None] ** 2 + k[None, :] ** 2)
    big *= np.tile(bump, (H, W))[..., None]
    big *= rng.normal(1.0, 0.04, size=big.shape[:2] + (1,)).astype(np.float32)
    rgba = np.empty(big.shape[:2] + (4,), dtype=np.uint8)
    rgba[..., :3] = np.clip(big, 0, 255)
    rgba[..., 3] = 255
    return rgba


def _plank_polygons(L, W):
    """Herringbone in a local frame: yields (corner list, orientation)."""
    for strip in range(-40, 41):
        for s in range(-60, 61):
            ox, oy = strip * L + s * W, -strip * L + s * W
            yield [(ox, oy), (ox + L, oy), (ox + L, oy + W), (ox, oy + W)], 0
            vx, vy = ox + L, oy + W - L
            yield [(vx, vy), (vx + W, vy), (vx + W, vy + L), (vx, vy + L)], 1


def generate_parquet(width, height, scale=1.0, seed=3):
    """Herringbone oak parquet rendered with cairo, rotated by 45 degrees."""
    import cairo

    width, height = int(width * scale), int(height * scale)
    rng = np.random.default_rng(seed)
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
    cr = cairo.Context(surface)
    cr.set_source_rgb(0.55, 0.36, 0.16)
    cr.paint()

    L, W = 150 * scale, 30 * scale
    diag = math.hypot(width, height)
    cr.translate(width / 2, height / 2)
    cr.rotate(math.pi / 4)
    tones = [(0.86, 0.62, 0.33), (0.80, 0.55, 0.27), (0.90, 0.68, 0.38),
             (0.76, 0.51, 0.25), (0.84, 0.60, 0.30)]
    for corners, vertical in _plank_polygons(L, W):
        xs = [c[0] for c in corners]
        ys = [c[1] for c in corners]
        if max(xs) < -diag or min(xs) > diag or max(ys) < -diag or min(ys) > diag:
            continue
        x0, y0 = min(xs), min(ys)
        w, h = max(xs) - x0, max(ys) - y0
        base = tones[rng.integers(len(tones))]
        jitter = rng.normal(0, 0.025)
        r, g, b = (min(1.0, c + jitter) for c in base)
        grad = (cairo.LinearGradient(x0, 0, x0 + w, 0) if not vertical
                else cairo.LinearGradient(0, y0, 0, y0 + h))
        grad.add_color_stop_rgb(0, r * 0.95, g * 0.95, b * 0.95)
        grad.add_color_stop_rgb(rng.uniform(0.3, 0.7), r, g, b)
        grad.add_color_stop_rgb(1, r * 0.93, g * 0.93, b * 0.93)
        cr.rectangle(x0, y0, w, h)
        cr.set_source(grad)
        cr.fill()
        # wood grain: thin wavy lines along the plank
        cr.set_line_width(0.7 * scale)
        for _ in range(int(rng.integers(3, 7))):
            t = rng.uniform(0.1, 0.9)
            amp = rng.uniform(0.5, 2.5) * scale
            cr.set_source_rgba(0.45, 0.28, 0.12, rng.uniform(0.08, 0.22))
            if not vertical:
                yy = y0 + t * h
                cr.move_to(x0, yy)
                cr.curve_to(x0 + w / 3, yy + amp, x0 + 2 * w / 3, yy - amp,
                            x0 + w, yy)
            else:
                xx = x0 + t * w
                cr.move_to(xx, y0)
                cr.curve_to(xx + amp, y0 + h / 3, xx - amp, y0 + 2 * h / 3,
                            xx, y0 + h)
            cr.stroke()
        # gaps between planks
        cr.rectangle(x0, y0, w, h)
        cr.set_line_width(1.1 * scale)
        cr.set_source_rgba(0.25, 0.14, 0.05, 0.55)
        cr.stroke()

    surface.flush()
    return cairo_to_rgba(surface)


def cairo_to_rgba(surface):
    """ARGB32 (premultiplied BGRA) cairo surface -> straight RGBA array."""
    w, h, stride = surface.get_width(), surface.get_height(), surface.get_stride()
    buf = np.frombuffer(surface.get_data(), dtype=np.uint8)
    buf = buf.reshape(h, stride)[:, : w * 4].reshape(h, w, 4)
    rgba = buf[..., [2, 1, 0, 3]].astype(np.float32)
    alpha = rgba[..., 3:4] / 255.0
    rgba[..., :3] = np.where(alpha > 0, rgba[..., :3] / np.maximum(alpha, 1e-6), 0)
    return np.clip(rgba, 0, 255).astype(np.uint8)


def load_image(path, max_size=None):
    """Load any image GdkPixbuf understands into an RGBA array."""
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    if max_size:
        pb = GdkPixbuf.Pixbuf.new_from_file_at_scale(str(path), max_size,
                                                     max_size, True)
    else:
        pb = GdkPixbuf.Pixbuf.new_from_file(str(path))
    return pixbuf_to_rgba(pb)


def pixbuf_to_rgba(pb):
    if not pb.get_has_alpha():
        pb = pb.add_alpha(False, 0, 0, 0)
    w, h, stride = pb.get_width(), pb.get_height(), pb.get_rowstride()
    data = np.frombuffer(pb.read_pixel_bytes().get_data(), dtype=np.uint8)
    rows = [data[r * stride: r * stride + w * 4] for r in range(h)]
    return np.stack(rows).reshape(h, w, 4).copy()


def cover(rgba, width, height):
    """Scale/crop an image so that it covers width x height (nearest)."""
    h, w = rgba.shape[:2]
    s = max(width / w, height / h)
    ys = np.clip(((np.arange(height) + 0.5) / s + (h - height / s) / 2).astype(int), 0, h - 1)
    xs = np.clip(((np.arange(width) + 0.5) / s + (w - width / s) / 2).astype(int), 0, w - 1)
    return rgba[ys][:, xs]
