"""Desktop items: files from the XDG desktop directory drawn on the floor."""

import math
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("GdkPixbuf", "2.0")
gi.require_version("Pango", "1.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Gdk, GdkPixbuf, Gio, GLib, Gtk, Pango, PangoCairo  # noqa: E402

from .textures import cairo_to_rgba  # noqa: E402

TILE_W, TILE_H = 96, 100       # logical size of an icon + its label
ICON_SIZE = 52
IMAGE_TYPES = ("image/png", "image/jpeg", "image/gif", "image/webp",
               "image/bmp", "image/svg+xml")


def desktop_dir(override=None):
    if override:
        return Path(override).expanduser()
    d = GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_DESKTOP)
    return Path(d) if d else Path.home() / "Desktop"


class DesktopItem:
    def __init__(self, path, x, y):
        self.path = Path(path)
        self.name = self.path.name
        self.x, self.y = x, y              # centre of the tile, logical px
        self.texture = None                # GL texture id, owned by renderer
        self.rgba = None                   # tile pixels waiting for upload

    def contains(self, px, py):
        return abs(px - self.x) <= TILE_W / 2 and abs(py - self.y) <= TILE_H / 2

    def open(self):
        uri = Gio.File.new_for_path(str(self.path)).get_uri()
        try:
            Gio.AppInfo.launch_default_for_uri(uri, None)
        except GLib.Error as err:
            print(f"carpeted: cannot open {self.path}: {err.message}")


def _icon_pixbuf(path, size, scale):
    gfile = Gio.File.new_for_path(str(path))
    px = size * scale
    try:
        info = gfile.query_info("standard::content-type,standard::icon,"
                                "thumbnail::path", Gio.FileQueryInfoFlags.NONE,
                                None)
    except GLib.Error:
        info = None

    if info is not None:
        ctype = info.get_content_type() or ""
        thumb = info.get_attribute_byte_string("thumbnail::path")
        for candidate in ([str(path)] if ctype in IMAGE_TYPES else []) + \
                ([thumb] if thumb else []):
            try:
                return GdkPixbuf.Pixbuf.new_from_file_at_scale(candidate, px, px,
                                                               True)
            except GLib.Error:
                pass

        display = Gdk.Display.get_default()
        gicon = info.get_icon()
        if display is not None and gicon is not None:
            theme = Gtk.IconTheme.get_for_display(display)
            paintable = theme.lookup_by_gicon(gicon, size, scale,
                                              Gtk.TextDirection.NONE,
                                              Gtk.IconLookupFlags(0))
            ifile = paintable.get_file() if paintable else None
            ipath = ifile.get_path() if ifile else None
            if ipath:
                try:
                    return GdkPixbuf.Pixbuf.new_from_file_at_scale(ipath, px, px,
                                                                   True)
                except GLib.Error:
                    pass
    return None


def _draw_paper(cr, x, y, s, folder):
    """Fallback icon when the icon theme gives us nothing usable."""
    if folder:
        cr.set_source_rgb(0.35, 0.68, 0.9)
        cr.rectangle(x + s * 0.08, y + s * 0.22, s * 0.84, s * 0.62)
        cr.fill()
        cr.rectangle(x + s * 0.08, y + s * 0.16, s * 0.34, s * 0.1)
        cr.fill()
        return
    fold = s * 0.22
    cr.move_to(x + s * 0.18, y + s * 0.06)
    cr.line_to(x + s * 0.82 - fold, y + s * 0.06)
    cr.line_to(x + s * 0.82, y + s * 0.06 + fold)
    cr.line_to(x + s * 0.82, y + s * 0.94)
    cr.line_to(x + s * 0.18, y + s * 0.94)
    cr.close_path()
    cr.set_source_rgb(0.97, 0.97, 0.96)
    cr.fill_preserve()
    cr.set_source_rgba(0, 0, 0, 0.25)
    cr.set_line_width(1)
    cr.stroke()
    cr.set_source_rgba(0, 0, 0, 0.18)
    for k in range(5):
        yy = y + s * (0.38 + k * 0.1)
        cr.rectangle(x + s * 0.28, yy, s * 0.44, s * 0.03)
    cr.fill()


def render_tile(item, scale=1):
    """Icon + label as a straight RGBA array of (TILE_H*scale, TILE_W*scale)."""
    import cairo

    w, h = TILE_W * scale, TILE_H * scale
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    cr = cairo.Context(surface)
    cr.scale(scale, scale)

    pb = _icon_pixbuf(item.path, ICON_SIZE, scale)
    ix, iy = (TILE_W - ICON_SIZE) / 2, 4
    if pb is not None:
        cr.save()
        pw, ph = pb.get_width() / scale, pb.get_height() / scale
        cr.translate(ix + (ICON_SIZE - pw) / 2, iy + (ICON_SIZE - ph) / 2)
        cr.scale(1 / scale, 1 / scale)
        Gdk.cairo_set_source_pixbuf(cr, pb, 0, 0)
        cr.paint()
        cr.restore()
    else:
        _draw_paper(cr, ix, iy, ICON_SIZE, item.path.is_dir())

    layout = PangoCairo.create_layout(cr)
    layout.set_font_description(Pango.FontDescription.from_string("Sans 9"))
    layout.set_width(int((TILE_W - 6) * Pango.SCALE))
    layout.set_alignment(Pango.Alignment.CENTER)
    layout.set_wrap(Pango.WrapMode.WORD_CHAR)
    layout.set_ellipsize(Pango.EllipsizeMode.END)
    layout.set_height(-2)                       # at most two lines
    layout.set_text(item.name, -1)
    ty = iy + ICON_SIZE + 4
    for dx, dy, alpha in ((0, 1, 0.9), (1, 1, 0.5), (-1, 1, 0.5), (0, 0, 0)):
        cr.move_to(3 + dx, ty + dy)
        if alpha:
            cr.set_source_rgba(0, 0, 0, alpha)
        else:
            cr.set_source_rgb(1, 1, 1)
        PangoCairo.show_layout(cr, layout)
    surface.flush()
    return cairo_to_rgba(surface)


class Desktop:
    """Keeps the list of items in sync with the desktop directory."""

    def __init__(self, directory, screen_size, positions=None, on_change=None):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.screen_w, self.screen_h = screen_size
        self.positions = dict(positions or {})
        self.pending_positions = {}
        self.on_change = on_change
        self.items = []
        self.monitor = Gio.File.new_for_path(str(self.directory)).monitor_directory(
            Gio.FileMonitorFlags.WATCH_MOVES, None)
        self.monitor.connect("changed", self._on_monitor)
        self.reload()

    def _free_slot(self, taken):
        """Next free cell in a column-major grid starting at the top right."""
        cols = max(1, int((self.screen_w - 40) // TILE_W))
        rows = max(1, int((self.screen_h - 40) // (TILE_H + 8)))
        for c in range(cols):
            for r in range(rows):
                x = self.screen_w - 30 - TILE_W / 2 - c * TILE_W
                y = 30 + TILE_H / 2 + r * (TILE_H + 8)
                if all(math.hypot(x - tx, y - ty) > TILE_W * 0.7
                       for tx, ty in taken):
                    return x, y
        return self.screen_w / 2, self.screen_h / 2

    def reload(self):
        try:
            names = sorted(p.name for p in self.directory.iterdir()
                           if not p.name.startswith("."))
        except OSError:
            names = []
        old = {it.name: it for it in self.items}
        items, taken = [], []
        for name in names:
            if name in old:
                item = old[name]
            else:
                pos = self.pending_positions.pop(name, None) or \
                    self.positions.get(name)
                item = DesktopItem(self.directory / name, *(pos or (None, None)))
            items.append(item)
            if item.x is not None:
                taken.append((item.x, item.y))
        for item in items:
            if item.x is None:
                item.x, item.y = self._free_slot(taken)
                taken.append((item.x, item.y))
        self.removed = [it for name, it in old.items() if name not in names]
        self.items = items
        self.positions = {it.name: (it.x, it.y) for it in items}

    def _on_monitor(self, *_):
        GLib.idle_add(self._reload_and_notify)

    def _reload_and_notify(self):
        self.reload()
        if self.on_change:
            self.on_change()
        return False

    def expect(self, name, x, y):
        """A file is about to appear (we are copying it) - place it here."""
        self.pending_positions[name] = (x, y)

    def item_at(self, x, y):
        for item in reversed(self.items):
            if item.contains(x, y):
                return item
        return None

    def raise_item(self, item):
        self.items.remove(item)
        self.items.append(item)

    def positions_state(self):
        return {it.name: [round(it.x, 1), round(it.y, 1)] for it in self.items}
