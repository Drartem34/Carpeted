project('carpeted', ['c', 'cpp'],
  version : '0.3.0',
  license : 'MIT',
  default_options : ['cpp_std=c++20', 'buildtype=release', 'warning_level=2',
                     'b_ndebug=if-release'])

wayland_protocols = dependency('wayland-protocols')
deps = [
  dependency('wayland-client'), dependency('wayland-egl'),
  dependency('wayland-cursor'),
  dependency('egl'), dependency('glesv2'),
  dependency('xkbcommon'),
  dependency('cairo'), dependency('pangocairo'),
  dependency('gdk-pixbuf-2.0'),
]

scanner = find_program(dependency('wayland-scanner', native : true)
  .get_variable(pkgconfig : 'wayland_scanner'))
wp_dir = wayland_protocols.get_variable(pkgconfig : 'pkgdatadir')

fs = import('fs')
tablet_xml = wp_dir / 'stable/tablet/tablet-v2.xml'
if not fs.exists(tablet_xml)
  tablet_xml = wp_dir / 'unstable/tablet/tablet-unstable-v2.xml'
endif

protocols = {
  'wlr-layer-shell-unstable-v1' : files('protocol/wlr-layer-shell-unstable-v1.xml'),
  'xdg-shell' : wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  'viewporter' : wp_dir / 'stable/viewporter/viewporter.xml',
  'fractional-scale-v1' : wp_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
  'cursor-shape-v1' : wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  'tablet-v2' : tablet_xml,
}

proto_src = []
foreach name, xml : protocols
  proto_src += custom_target(name + '-header',
    input : xml, output : name + '-client-protocol.h',
    command : [scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  proto_src += custom_target(name + '-code',
    input : xml, output : name + '-protocol.c',
    command : [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

executable('carpeted',
  'src/main.cpp', 'src/daemon.cpp', 'src/app.cpp', 'src/wayland.cpp', 'src/renderer.cpp',
  'src/cloth.cpp', 'src/rugtex.cpp', 'src/cards.cpp', 'src/shelf.cpp',
  'src/hypr.cpp', 'src/util.cpp', 'src/json.cpp',
  proto_src,
  dependencies : deps,
  install : true)

test_exe = executable('carpeted-tests',
  'tests/tests.cpp', 'src/cloth.cpp', 'src/json.cpp')
test('unit', test_exe)
