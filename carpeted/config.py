"""Configuration: defaults <- ~/.config/carpeted/config.toml <- command line."""

import tomllib
from pathlib import Path

from gi.repository import GLib

DEFAULTS = {
    "mode": "layer",             # "layer" (desktop) or "window"
    "layer": "bottom",           # "bottom" or "background"
    "output": None,              # monitor connector, e.g. "DP-1"
    "floor": "parquet",          # "parquet", "none" (see-through) or image path
    "rug_image": None,           # use your own rug picture instead
    "rug_width": 760,
    "rug_height": 480,
    "rug_auto_size": True,       # shrink the rug on small screens
    "rug_position": None,        # [x, y] of the top-left corner
    "rug_cell": 19,              # physics grid spacing in px (smaller = slower)
    "solver_iterations": 10,
    "lift_height": 70,           # how high a grabbed rug point is lifted
    "focal": 1800,               # perspective strength (smaller = stronger)
    "desktop_dir": None,         # defaults to XDG_DESKTOP_DIR
    "drop_action": "copy",       # files dropped from a file manager: copy/move
    "window_size": [1280, 800],
}


def config_path():
    return Path(GLib.get_user_config_dir()) / "carpeted" / "config.toml"


def load(path=None):
    cfg = dict(DEFAULTS)
    path = Path(path) if path else config_path()
    if path.exists():
        with open(path, "rb") as f:
            user = tomllib.load(f)
        unknown = set(user) - set(DEFAULTS)
        if unknown:
            print(f"carpeted: unknown config keys ignored: {', '.join(sorted(unknown))}")
        cfg.update({k: v for k, v in user.items() if k in DEFAULTS})
    return cfg
