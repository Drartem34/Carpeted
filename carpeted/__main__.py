"""Entry point.  `python -m carpeted --help`."""

import argparse
import ctypes.util
import json
import os
import sys
from pathlib import Path

LAYER_SHELL_LIBS = [
    "/usr/lib/libgtk4-layer-shell.so",
    "/usr/lib64/libgtk4-layer-shell.so",
    "/usr/lib/x86_64-linux-gnu/libgtk4-layer-shell.so",
    "/usr/lib/aarch64-linux-gnu/libgtk4-layer-shell.so",
    "/usr/local/lib/libgtk4-layer-shell.so",
    "/usr/local/lib64/libgtk4-layer-shell.so",
]


def _preload_layer_shell():
    """gtk4-layer-shell must be loaded before libwayland-client, which means
    LD_PRELOAD.  Re-exec ourselves with it if needed."""
    if "libgtk4-layer-shell" in os.environ.get("LD_PRELOAD", "") or \
            os.environ.get("CARPETED_NO_PRELOAD"):
        return
    lib = None
    for candidate in LAYER_SHELL_LIBS:
        for path in (candidate, candidate + ".0"):
            if os.path.exists(path):
                lib = path
                break
        if lib:
            break
    if lib is None:
        name = ctypes.util.find_library("gtk4-layer-shell")
        lib = name if name and os.path.isabs(name) else None
    if lib is None:
        return                      # app.py will warn and fall back to a window
    env = dict(os.environ)
    env["LD_PRELOAD"] = (lib + " " + env.get("LD_PRELOAD", "")).strip()
    env["CARPETED_NO_PRELOAD"] = "1"
    package_root = str(Path(__file__).resolve().parent.parent)
    env["PYTHONPATH"] = os.pathsep.join(
        p for p in (package_root, env.get("PYTHONPATH")) if p)
    os.execve(sys.executable, [sys.executable, "-m", "carpeted", *sys.argv[1:]], env)


def main():
    parser = argparse.ArgumentParser(
        prog="carpeted",
        description="A rug with real cloth physics on your desktop. "
                    "Lift a corner, push files under it.")
    parser.add_argument("--window", action="store_true",
                        help="run in a normal window instead of on the desktop")
    parser.add_argument("--layer", choices=["bottom", "background"],
                        help="layer-shell layer (default: bottom)")
    parser.add_argument("--output", help="monitor to use, e.g. DP-1")
    parser.add_argument("--floor", help='"parquet", "none" or an image path')
    parser.add_argument("--rug-image", help="picture of a rug to use")
    parser.add_argument("--desktop-dir", help="folder whose files lie on the floor")
    parser.add_argument("--config", help="config file (default "
                        "~/.config/carpeted/config.toml)")
    parser.add_argument("--reset", action="store_true",
                        help="put the rug back in the middle (also works on a "
                             "running instance)")
    parser.add_argument("--shake", action="store_true",
                        help="shake out the rug of a running instance")
    parser.add_argument("--quit", action="store_true",
                        help="quit the running instance")
    # development helpers
    parser.add_argument("--screenshot", help=argparse.SUPPRESS)
    parser.add_argument("--frames", type=int, default=2, help=argparse.SUPPRESS)
    parser.add_argument("--script", help=argparse.SUPPRESS)
    parser.add_argument("--no-save", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()

    if not args.window and os.environ.get("WAYLAND_DISPLAY"):
        _preload_layer_shell()
    # GTK draws through EGL; PyOpenGL has to use the same platform.
    os.environ.setdefault("PYOPENGL_PLATFORM", "egl")

    from . import config
    cfg = config.load(args.config)
    if args.window or not os.environ.get("WAYLAND_DISPLAY"):
        cfg["mode"] = "window"
    for key in ("layer", "output", "floor", "rug_image", "desktop_dir"):
        value = getattr(args, key)
        if value is not None:
            cfg[key] = value
    if args.screenshot or args.script:
        cfg["debug"] = {"screenshot": args.screenshot, "frames": args.frames,
                        "script": json.loads(Path(args.script).read_text())
                        if args.script else None}
        cfg["no_save"] = True
    if args.no_save:
        cfg["no_save"] = True

    from .app import CarpetedApp
    app = CarpetedApp(cfg)
    forwarded = [a for a in ("--reset", "--shake", "--quit") if getattr(args, a[2:])]
    return app.run([sys.argv[0], *forwarded])


if __name__ == "__main__":
    sys.exit(main())
