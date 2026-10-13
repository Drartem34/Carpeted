"""GTK4 application: a layer-shell surface (or a plain window) with a rug."""

import json
import shutil
import sys
from pathlib import Path

import numpy as np
import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from . import textures  # noqa: E402
from .cloth import Cloth  # noqa: E402
from .desktop import ICON_SIZE, TILE_H, TILE_W, Desktop, desktop_dir, render_tile  # noqa: E402
from .renderer import Renderer, project, unproject  # noqa: E402

try:
    gi.require_version("Gtk4LayerShell", "1.0")
    from gi.repository import Gtk4LayerShell as LayerShell  # noqa: E402
except (ValueError, ImportError):
    LayerShell = None

STEP = 1.0 / 60.0


def state_path(output):
    base = Path(GLib.get_user_state_dir()) if hasattr(GLib, "get_user_state_dir") \
        else Path.home() / ".local/state"
    name = f"state-{output}.json" if output else "state.json"
    return base / "carpeted" / name


class Scene:
    """Everything that lives on the floor: the rug and the desktop items."""

    def __init__(self, cfg, size, saved):
        self.cfg = cfg
        self.size = size
        w, h = cfg["rug_width"], cfg["rug_height"]
        cols = max(8, int(round(w / cfg["rug_cell"]))) + 1
        rows = max(6, int(round(h / cfg["rug_cell"]))) + 1
        origin = cfg.get("rug_position") or ((size[0] - w) / 2, (size[1] - h) / 2)
        self.cloth = Cloth(w, h, cols, rows, origin)
        self.cloth.iterations = cfg["solver_iterations"]
        if saved.get("rug"):
            self.cloth.load_state(saved["rug"])
        self.cloth.compute_normals()

    # --------------------------------------------------------------- picking
    def pick_rug(self, x, y):
        """Top-most rug particle under screen point (x, y), or None."""
        c = self.cloth
        sp = project(c.pos, self.size, self.cfg["focal"])
        tri = c.triangles
        a, b, d = sp[tri[:, 0]], sp[tri[:, 1]], sp[tri[:, 2]]
        p = np.array([x, y])

        def cross(o, u, v):
            return (u[:, 0] - o[:, 0]) * (v[1] - o[:, 1]) - \
                (u[:, 1] - o[:, 1]) * (v[0] - o[:, 0])

        c1, c2, c3 = cross(a, b, p), cross(b, d, p), cross(d, a, p)
        inside = ((c1 >= 0) & (c2 >= 0) & (c3 >= 0)) | \
            ((c1 <= 0) & (c2 <= 0) & (c3 <= 0))
        hits = np.nonzero(inside)[0]
        if hits.size == 0:
            return None
        heights = c.pos[tri[hits], 2].mean(axis=1)
        best = tri[hits[np.argmax(heights)]]
        dist = np.linalg.norm(sp[best] - p, axis=1)
        return int(best[np.argmin(dist)])

    def covered(self, x, y):
        return self.pick_rug(x, y) is not None


class CarpetedWindow(Gtk.ApplicationWindow):
    def __init__(self, app, cfg):
        super().__init__(application=app, title="Carpeted")
        self.cfg = cfg
        self.output = cfg.get("output")
        self.scene = None
        self.desktop = None
        self.renderer = None
        self.saved = self._load_state()
        self.mouse = (0.0, 0.0)
        self.grab = None            # particle index while dragging the rug
        self.grab_z = 0.0
        self.carry = None           # (item, dx, dy) while dragging an icon
        self.selected = None
        self.tick_id = None
        self.last_time = None
        self.accum = 0.0
        self.dirty_state = False
        self.tiles_scale = None
        self.floor_size = None
        self.debug = cfg.get("debug") or {}
        self.frame_no = 0

        self.add_css_class("carpeted")
        css = Gtk.CssProvider()
        css.load_from_string("window.carpeted { background: transparent; }") \
            if hasattr(css, "load_from_string") else \
            css.load_from_data(b"window.carpeted { background: transparent; }")
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        if cfg["mode"] == "layer":
            self._setup_layer_shell()
        else:
            self.set_default_size(*cfg["window_size"])

        area = self.area = Gtk.GLArea()
        area.set_has_depth_buffer(True)
        if hasattr(area, "set_allowed_apis"):
            area.set_allowed_apis(Gdk.GLAPI.GL)
        area.set_required_version(3, 3)
        area.set_auto_render(False)
        area.set_hexpand(True)
        area.set_vexpand(True)
        area.connect("realize", self._on_realize)
        area.connect("unrealize", self._on_unrealize)
        area.connect("render", self._on_render)
        area.connect("resize", self._on_resize)
        self.set_child(area)

        # GestureDrag drives press/drag/release (a GestureClick gets cancelled
        # once the pointer moves too far); GestureClick only detects
        # double clicks.
        drag = Gtk.GestureDrag(button=1)
        drag.connect("drag-begin", lambda g, x, y: self._on_pressed(x, y))
        drag.connect("drag-update", self._on_drag_update)
        drag.connect("drag-end", lambda g, dx, dy: self._on_released())
        area.add_controller(drag)
        click = Gtk.GestureClick(button=1)
        click.connect("pressed", self._on_click)
        area.add_controller(click)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._on_motion)
        area.add_controller(motion)
        drop = Gtk.DropTarget.new(Gdk.FileList, Gdk.DragAction.COPY | Gdk.DragAction.MOVE)
        drop.connect("drop", self._on_drop)
        area.add_controller(drop)

        self.connect("close-request", lambda *_: self.save_state() or False)

    # ------------------------------------------------------------ layer shell
    def _setup_layer_shell(self):
        if LayerShell is None or not LayerShell.is_supported():
            print("carpeted: gtk4-layer-shell is not available, "
                  "falling back to a normal window", file=sys.stderr)
            self.cfg["mode"] = "window"
            self.set_default_size(*self.cfg["window_size"])
            return
        LayerShell.init_for_window(self)
        layer = {"background": LayerShell.Layer.BACKGROUND,
                 "bottom": LayerShell.Layer.BOTTOM}[self.cfg["layer"]]
        LayerShell.set_layer(self, layer)
        LayerShell.set_namespace(self, "carpeted")
        for edge in (LayerShell.Edge.LEFT, LayerShell.Edge.RIGHT,
                     LayerShell.Edge.TOP, LayerShell.Edge.BOTTOM):
            LayerShell.set_anchor(self, edge, True)
        LayerShell.set_exclusive_zone(self, -1)
        LayerShell.set_keyboard_mode(self, LayerShell.KeyboardMode.NONE)
        if self.output:
            monitors = Gdk.Display.get_default().get_monitors()
            for i in range(monitors.get_n_items()):
                mon = monitors.get_item(i)
                if mon.get_connector() == self.output:
                    LayerShell.set_monitor(self, mon)
                    break
            else:
                print(f"carpeted: output {self.output} not found", file=sys.stderr)

    # ------------------------------------------------------------------ state
    def _load_state(self):
        try:
            return json.loads(state_path(self.output).read_text())
        except (OSError, ValueError):
            return {}

    def save_state(self):
        if self.scene is None or self.cfg.get("no_save"):
            return
        data = {"rug": self.scene.cloth.state(),
                "icons": self.desktop.positions_state() if self.desktop else {}}
        path = state_path(self.output)
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_suffix(".tmp")
            tmp.write_text(json.dumps(data))
            tmp.replace(path)
        except OSError as err:
            print(f"carpeted: cannot save state: {err}", file=sys.stderr)
        self.dirty_state = False

    # -------------------------------------------------------------- GL hooks
    def _on_realize(self, area):
        area.make_current()
        if area.get_error() is not None:
            print(f"carpeted: OpenGL error: {area.get_error().message}",
                  file=sys.stderr)
            return
        rug = None
        if self.cfg.get("rug_image"):
            try:
                rug = textures.load_image(Path(self.cfg["rug_image"]).expanduser())
            except Exception as err:  # noqa: BLE001 - any loader failure
                print(f"carpeted: cannot load rug image: {err}", file=sys.stderr)
        if rug is None:
            rug = textures.generate_rug()
        self._rug_rgba = rug

    def _on_unrealize(self, area):
        self.save_state()
        self._stop_ticking()

    def _on_resize(self, area, width, height):
        size = (area.get_width(), area.get_height())
        if size[0] <= 0 or size[1] <= 0 or not hasattr(self, "_rug_rgba"):
            return
        if self.scene is None:
            if self.cfg.get("rug_auto_size", True):
                rw = min(self.cfg["rug_width"], size[0] * 0.55)
                rh = rw * self.cfg["rug_height"] / self.cfg["rug_width"]
                self.cfg["rug_width"], self.cfg["rug_height"] = rw, rh
            self.scene = Scene(self.cfg, size, self.saved)
            self.renderer = Renderer(self.scene.cloth, self._rug_rgba,
                                     self.cfg["focal"])
            area.make_current()
            self.renderer.realize()
            self.desktop = Desktop(desktop_dir(self.cfg.get("desktop_dir")), size,
                                   self.saved.get("icons"), self._on_desktop_changed)
            self._update_obstacles()
        else:
            self.scene.size = size
            self.desktop.screen_w, self.desktop.screen_h = size
        self._ensure_floor(size)
        self._ensure_tiles()
        self._wake()

    def _ensure_floor(self, size):
        scale = self.area.get_scale_factor()
        key = (size, scale)
        if self.floor_size == key:
            return
        self.floor_size = key
        floor = self.cfg["floor"]
        if floor == "none":
            rgba = None
        elif floor == "parquet":
            rgba = textures.generate_parquet(size[0], size[1], scale)
        else:
            try:
                img = textures.load_image(Path(floor).expanduser())
                rgba = textures.cover(img, size[0] * scale, size[1] * scale)
            except Exception as err:  # noqa: BLE001
                print(f"carpeted: cannot load floor image: {err}", file=sys.stderr)
                rgba = textures.generate_parquet(size[0], size[1], scale)
        self.area.make_current()
        self.renderer.set_floor(rgba)

    def _ensure_tiles(self, force=False):
        scale = self.area.get_scale_factor()
        rerender = force or scale != self.tiles_scale
        self.tiles_scale = scale
        for item in self.desktop.items:
            if rerender or item.texture is None:
                item.rgba = render_tile(item, scale)

    def _on_desktop_changed(self):
        if self.renderer and self.desktop.removed:
            self.area.make_current()
            self.renderer.forget_items(self.desktop.removed)
            if self.selected in self.desktop.removed:
                self.selected = None
        self._ensure_tiles()
        self._update_obstacles()
        self.dirty_state = True
        self._wake()

    def _update_obstacles(self):
        rects = []
        for it in self.desktop.items:
            if self.carry and it is self.carry[0]:
                continue
            top = it.y - TILE_H / 2 + 4
            rects.append((it.x - ICON_SIZE / 2, top, it.x + ICON_SIZE / 2,
                          top + ICON_SIZE))
        self.scene.cloth.set_obstacles(rects)
        if not self.scene.cloth.sleeping:
            self._wake()

    def _on_render(self, area, context):
        if self.renderer is None:
            return True
        size = (area.get_width(), area.get_height())
        carried = self.carry[0] if self.carry else None
        self.renderer.render(size, self.desktop.items, self.selected, carried,
                             transparent=self.cfg["floor"] == "none")
        self.frame_no += 1
        shot = self.debug.get("screenshot")
        if shot and self.frame_no >= self.debug.get("frames", 2):
            scale = area.get_scale_factor()
            img = self.renderer.read_pixels(size[0] * scale, size[1] * scale)
            self._save_png(img, shot)
            self.get_application().quit()
        return True

    @staticmethod
    def _save_png(img, path):
        gi.require_version("GdkPixbuf", "2.0")
        from gi.repository import GdkPixbuf
        h, w = img.shape[:2]
        pb = GdkPixbuf.Pixbuf.new_from_bytes(GLib.Bytes.new(img.tobytes()),
                                             GdkPixbuf.Colorspace.RGB, True, 8,
                                             w, h, w * 4)
        pb.savev(path, "png", [], [])

    # ---------------------------------------------------------- frame ticks
    def _wake(self):
        if self.scene is not None:
            self.scene.cloth.wake()
        if self.tick_id is None:
            self.last_time = None
            self.tick_id = self.area.add_tick_callback(self._on_tick)
        self.area.queue_render()

    def _stop_ticking(self):
        if self.tick_id is not None:
            self.area.remove_tick_callback(self.tick_id)
            self.tick_id = None

    def _on_tick(self, area, clock):
        now = clock.get_frame_time() / 1e6
        if self.debug.get("script") is not None:
            self._run_script()
            dt = STEP                                   # deterministic tests
        else:
            dt = 0.0 if self.last_time is None else min(now - self.last_time, 0.1)
        self.last_time = now
        self.accum += dt
        cloth = self.scene.cloth
        steps = 0
        while self.accum >= STEP and steps < 3:
            self._update_grab(STEP)
            cloth.step(STEP)
            self.accum -= STEP
            steps += 1
        if steps == 3:
            self.accum = 0.0
        if steps:
            self._update_obstacles()
        area.queue_render()
        if cloth.sleeping and self.carry is None and not self.debug:
            if self.dirty_state:
                self.save_state()
            self.tick_id = None
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    def _update_grab(self, dt):
        if self.grab is None:
            return
        lift = self.cfg["lift_height"]
        self.grab_z = min(lift, self.grab_z + lift * dt * 6)
        x, y = unproject(*self.mouse, self.grab_z, self.scene.size,
                         self.cfg["focal"])
        self.scene.cloth.pin(self.grab, (x, y, self.grab_z))

    # -------------------------------------------------------------- input
    def _on_motion(self, ctrl, x, y):
        self.mouse = (x, y)
        if self.scene is None:
            return
        if self.carry:
            item, dx, dy = self.carry
            item.x, item.y = x + dx, y + dy
            self.area.queue_render()
        elif self.grab is None:
            self.area.set_cursor_from_name(
                "grab" if self.scene.covered(x, y) else None)

    def _on_drag_update(self, gesture, off_x, off_y):
        ok, x, y = gesture.get_start_point()
        if ok:
            self._on_motion(None, x + off_x, y + off_y)

    def _on_pressed(self, x, y):
        if self.scene is None:
            return
        self.mouse = (x, y)
        hit = self.scene.pick_rug(x, y)
        if hit is not None:
            self.grab = hit
            self.grab_z = float(self.scene.cloth.pos[hit, 2])
            self.area.set_cursor_from_name("grabbing")
            self._wake()
            return
        item = self.desktop.item_at(x, y)
        self.selected = item
        if item is not None:
            self.desktop.raise_item(item)
            self.carry = (item, item.x - x, item.y - y)
            self._update_obstacles()
            self._wake()
        self.area.queue_render()

    def _on_click(self, gesture, n_press, x, y):
        if n_press != 2 or self.scene is None:
            return
        if self.scene.pick_rug(x, y) is not None:
            self.grab = None
            self.shake_rug()
            return
        item = self.desktop.item_at(x, y)
        if item is not None:
            item.open()

    def _on_released(self):
        if self.grab is not None:
            self.grab = None
            self.scene.cloth.unpin_all()
            self.area.set_cursor_from_name("grab")
            self.dirty_state = True
            self._wake()
        if self.carry:
            item = self.carry[0]
            w, h = self.scene.size
            item.x = float(np.clip(item.x, TILE_W / 2, w - TILE_W / 2))
            item.y = float(np.clip(item.y, TILE_H / 2, h - TILE_H / 2))
            self.carry = None
            self.desktop.positions = {it.name: (it.x, it.y)
                                      for it in self.desktop.items}
            self.dirty_state = True
            self._update_obstacles()
            self._wake()

    def _on_drop(self, target, value, x, y):
        files = value.get_files() if hasattr(value, "get_files") else []
        if not files:
            return False
        move = self.cfg["drop_action"] == "move"
        dest_dir = self.desktop.directory
        for k, gfile in enumerate(files):
            src = gfile.get_path()
            if not src:
                continue
            src = Path(src)
            if src.parent == dest_dir:          # already on the desktop
                for it in self.desktop.items:
                    if it.path == src:
                        it.x, it.y = x + k * 12, y + k * 12
                continue
            dest = dest_dir / src.name
            n = 1
            while dest.exists():
                dest = dest_dir / f"{src.stem} ({n}){src.suffix}"
                n += 1
            self.desktop.expect(dest.name, x + k * 12, y + k * 12)
            try:
                if move:
                    shutil.move(str(src), str(dest))
                elif src.is_dir():
                    shutil.copytree(src, dest, symlinks=True)
                else:
                    shutil.copy2(src, dest)
            except OSError as err:
                print(f"carpeted: cannot put {src} on the floor: {err}",
                      file=sys.stderr)
        self.desktop.reload()
        self._on_desktop_changed()
        return True

    # ------------------------------------------------------------- actions
    def shake_rug(self):
        """Pick the rug up and drop it back flat (straightens folds)."""
        if self.scene is None:
            return
        c = self.scene.cloth
        cx, cy, _ = c.centroid()
        c.unpin_all()
        c.reset_flat((cx - c.width / 2, cy - c.height / 2), z=40.0)
        self.dirty_state = True
        self._wake()

    def reset_rug(self):
        if self.scene is None:
            return
        c = self.scene.cloth
        w, h = self.scene.size
        c.unpin_all()
        c.reset_flat(((w - c.width) / 2, (h - c.height) / 2), z=60.0)
        self.dirty_state = True
        self._wake()

    # --------------------------------------------------------- test scripts
    def _run_script(self):
        script = self.debug["script"]
        t = self.frame_no
        for when, action, *args in list(script):
            if when > t:
                continue
            script.remove([when, action, *args])
            if action == "press":
                self._on_pressed(*args)
            elif action == "move":
                self._on_motion(None, *args)
            elif action == "release":
                self._on_motion(None, *args)
                self._on_released()
            elif action == "shake":
                self.shake_rug()
        if not script and self.scene.cloth.sleeping and \
                not self.debug.get("screenshot"):
            self.get_application().quit()


class CarpetedApp(Gtk.Application):
    def __init__(self, cfg):
        app_id = "io.github.carpeted"
        if cfg.get("output"):
            app_id += "." + "".join(ch if ch.isalnum() else "_"
                                    for ch in cfg["output"])
        flags = Gio.ApplicationFlags.HANDLES_COMMAND_LINE
        if cfg.get("debug"):
            flags |= Gio.ApplicationFlags.NON_UNIQUE
        super().__init__(application_id=app_id, flags=flags)
        self.cfg = cfg
        self.window = None
        for sig in (2, 15):                     # SIGINT, SIGTERM
            GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, self._on_signal)

    def _on_signal(self):
        if self.window is not None:
            self.window.save_state()
        self.quit()
        return GLib.SOURCE_REMOVE

    def do_command_line(self, command_line):
        args = command_line.get_arguments()[1:]
        if self.window is None:
            if "--quit" in args or "--shake" in args:
                return 0                 # nothing is running - nothing to do
            self.window = CarpetedWindow(self, self.cfg)
            self.window.present()
            if "--reset" in args:
                GLib.timeout_add(500, lambda: self.window.reset_rug() or False)
            return 0
        if "--quit" in args:
            self.window.save_state()
            self.quit()
        elif "--reset" in args:
            self.window.reset_rug()
        elif "--shake" in args:
            self.window.shake_rug()
        return 0
