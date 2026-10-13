#!/bin/sh
# Installs carpeted for the current user: ~/.local/share/carpeted + ~/.local/bin/carpeted
set -e
here=$(cd "$(dirname "$0")" && pwd)
dest="${XDG_DATA_HOME:-$HOME/.local/share}/carpeted"
bin="$HOME/.local/bin"

python3 - <<'PY' || { echo; echo "Missing dependencies, see README.md (\"Встановлення\")."; exit 1; }
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk  # noqa
import cairo, numpy, OpenGL  # noqa
try:
    gi.require_version("Gtk4LayerShell", "1.0")
except ValueError:
    print("warning: gtk4-layer-shell not found - carpeted will only work with --window")
PY

mkdir -p "$dest" "$bin"
rm -rf "$dest/carpeted"
cp -r "$here/carpeted" "$dest/"
cat > "$bin/carpeted" <<SH
#!/bin/sh
PYTHONPATH="$dest\${PYTHONPATH:+:\$PYTHONPATH}" exec python3 -m carpeted "\$@"
SH
chmod +x "$bin/carpeted"
echo "Installed to $dest"
echo "Run: carpeted   (make sure $bin is in PATH)"
