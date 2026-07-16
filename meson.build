project(
  'nvland',
  'c',
  meson_version : '>= 1.3.0',
  version : '0.1',
  default_options : ['warning_level=3'],
)

wayland_protos = dependency('wayland-protocols', version: '>=1.14')
wl_scanner = find_program('wayland-scanner')

# Get the path to the XML definition
wl_protocol_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')
xdg_shell_xml = wl_protocol_dir + '/stable/xdg-shell/xdg-shell.xml'

# Instruct meson to generate the header file
xdg_shell_proto_h = custom_target(
  'xdg-shell-protocol.h',
  output: 'xdg-shell-protocol.h',
  input: xdg_shell_xml,
  command: [wl_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

dependencies = [
  dependency('wlroots-0.19'),
  dependency('wayland-server'),
  dependency('xkbcommon'),
  dependency('cairo'),
  dependency('mpack'),
]

sources = [
  'nvland.c',
  xdg_shell_proto_h
]

exe = executable(
  'nvland',
  sources,
  dependencies : dependencies,
  # install : true,
  c_args : '-DWLR_USE_UNSTABLE'
)
