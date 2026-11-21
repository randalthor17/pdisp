project('pdisp', 'c',
  version: '0.0.1',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true', 'b_ndebug=false',
                    'buildtype=debugoptimized'])

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

m_dep = meson.get_compiler('c').find_library('m')
evdi_dep = dependency('evdi')

inc = include_directories('daemon', 'tools', 'proto')

edid_lib = static_library('pdisp-edid', 'daemon/edid.c',
  include_directories: inc,
  dependencies: m_dep)
edid_dep = declare_dependency(link_with: edid_lib, include_directories: inc)

vdisplay_lib = static_library('pdisp-vdisplay', 'daemon/vdisplay.c',
  include_directories: inc,
  dependencies: [evdi_dep, edid_dep])
vdisplay_dep = declare_dependency(
  link_with: vdisplay_lib, include_directories: inc, dependencies: [evdi_dep, edid_dep])

edid_cli_lib = static_library('pdisp-edid-cli', 'tools/edid_cli.c', dependencies: edid_dep)
edid_cli_dep = declare_dependency(link_with: edid_cli_lib, dependencies: edid_dep)

# Wayland client (test pattern) and capture/PNG tools
zlib_dep = dependency('zlib')
wayland_client_dep = dependency('wayland-client')
wayland_scanner = find_program('wayland-scanner')
xdg_xml = dependency('wayland-protocols').get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_h = custom_target('xdg-shell-client-header',
  input: xdg_xml, output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
xdg_shell_c = custom_target('xdg-shell-protocol-code',
  input: xdg_xml, output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

libva_dep = dependency('libva')
libva_drm_dep = dependency('libva-drm')

encoder_lib = static_library('pdisp-encoder', 'daemon/encoder.c', 'daemon/hevc_headers.c',
  include_directories: inc,
  dependencies: [libva_dep, libva_drm_dep])
encoder_dep = declare_dependency(
  link_with: encoder_lib, include_directories: inc, dependencies: [libva_dep, libva_drm_dep])

pdisp_edid = executable('pdisp-edid', 'tools/pdisp-edid.c', dependencies: edid_cli_dep)
pdisp_pattern = executable('pdisp-pattern', 'tools/pdisp-pattern.c', xdg_shell_h, xdg_shell_c,
  include_directories: inc, dependencies: wayland_client_dep)
pdisp_capture = executable('pdisp-capture', 'tools/pdisp-capture.c', 'tools/png.c',
  dependencies: [edid_cli_dep, vdisplay_dep, zlib_dep])

pdisp_encode = executable('pdisp-encode', 'tools/pdisp-encode.c',
  dependencies: [edid_cli_dep, vdisplay_dep, encoder_dep])

pdisp_vdisplay = executable('pdisp-vdisplay', 'tools/pdisp-vdisplay.c', dependencies: [edid_cli_dep, vdisplay_dep])

test_edid = executable('test_edid', 'tests/test_edid.c', dependencies: [edid_dep, m_dep])
test('edid unit', test_edid)

# Conformance check of generated EDIDs against edid-decode (skipped if it is not installed).
edid_decode = find_program('edid-decode', required: false)
if edid_decode.found()
  edid_check = find_program('tests/edid_decode_check.sh')
  test('edid-decode conformance', edid_check,
    args: [pdisp_edid, edid_decode],
    protocol: 'exitcode')
endif

# Integration check against a live niri session and a free evdi device (skipped when they are missing).
m1_check = find_program('tests/m1_niri_check.sh')
test('m1 niri output', m1_check,
  args: [pdisp_vdisplay],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 60,
  suite: 'integration')

m2_check = find_program('tests/m2_capture_check.sh')
test('m2 capture round trip', m2_check,
  args: [pdisp_capture, pdisp_pattern, meson.current_build_dir() / 'm2-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m3_check = find_program('tests/m3_encode_check.sh')
test('m3 encode', m3_check,
  args: [pdisp_encode, pdisp_pattern, meson.current_build_dir() / 'm3-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

test_hevc_headers = executable('test_hevc_headers', 'tests/test_hevc_headers.c', 'daemon/hevc_headers.c',
  include_directories: inc)
test('hevc headers unit', test_hevc_headers)

# synthetic-frame encoder driver used by the loss recovery test
enc_synth = executable('enc_synth', 'tests/enc_synth.c', dependencies: [encoder_dep], include_directories: inc)

test('loss recovery', find_program('tests/loss_recovery_check.sh'),
  args: [enc_synth, meson.current_build_dir() / 'loss-artifacts'],
  protocol: 'exitcode',
  timeout: 120,
  suite: 'integration')

proto_lib = static_library('pdisp-proto', 'proto/proto.c', include_directories: inc)
proto_dep = declare_dependency(link_with: proto_lib, include_directories: inc)
test_proto = executable('test_proto', 'tests/test_proto.c', dependencies: proto_dep)
test('proto wire format', test_proto)
