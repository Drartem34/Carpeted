# Build and install on Arch:  makepkg -si   (run in this folder)
pkgname=carpeted
pkgver=0.3.0
pkgrel=1
pkgdesc="A rug with real cloth physics that rolls out from the bottom of the screen; hide files, links and windows under it"
arch=('x86_64' 'aarch64')
url="https://github.com/drartem34/carpeted"
license=('MIT')
depends=('wayland' 'libglvnd' 'libxkbcommon' 'cairo' 'pango' 'gdk-pixbuf2')
makedepends=('meson' 'ninja' 'wayland-protocols')
optdepends=('grim: thumbnails of windows put on the rug')

build() {
  arch-meson "$startdir" build
  meson compile -C build
}

check() {
  meson test -C build
}

package() {
  meson install -C build --destdir "$pkgdir"
  install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 "$startdir/contrib/hyprland.conf" "$pkgdir/usr/share/doc/$pkgname/hyprland.conf"
  install -Dm644 "$startdir/contrib/config.example" "$pkgdir/usr/share/doc/$pkgname/config.example"
}
