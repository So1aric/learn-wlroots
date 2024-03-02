project('learn-wlroots', 'c')

cc = meson.get_compiler('c')

wl_server = dependency('wayland-server')
wl_protos = dependency('wayland-protocols')
wl_scannr = dependency('wayland-scanner')
wl_scannr_prog = find_program(wl_scannr.get_variable('wayland_scanner'), native: true)

wl_scannr_code = generator(
  wl_scannr_prog,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wl_scannr_head = generator(
  wl_scannr_prog,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

wl_proto_dir = wl_protos.get_variable('pkgdatadir')
wl_proto_files = [
  wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml',
]

wl_proto_src = []
foreach filename: wl_proto_files
  wl_proto_src += wl_scannr_code.process(filename)
  wl_proto_src += wl_scannr_head.process(filename)
endforeach

math = cc.find_library('m')

wlroots = cc.find_library('wlroots')

pixman = dependency('pixman-1')
xkbcommon = dependency('xkbcommon')

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
], language: 'c')

executable(
  'main',
  [
    'src/main.c',
    wl_proto_src,
  ],
  dependencies: [
    math,
    wl_server,
    wlroots,
    pixman,
    xkbcommon,
  ],
  include_directories: [
    'include',
  ],
)
