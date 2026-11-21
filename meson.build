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

mc_inc = include_directories('third_party/monocypher')
monocypher = static_library('monocypher', 'third_party/monocypher/monocypher.c', c_args: ['-w'])
proto_lib = static_library('pdisp-proto', 'proto/proto.c', 'proto/fec.c', 'proto/video.c', 'proto/vsend.c', 'proto/vrecv.c', 'proto/net.c', 'proto/timesync.c', 'proto/usbmux.c', 'proto/crypto.c', include_directories: [inc, mc_inc], link_with: monocypher)
proto_dep = declare_dependency(link_with: proto_lib, include_directories: inc)
test_proto = executable('test_proto', 'tests/test_proto.c', dependencies: proto_dep)
test('proto wire format', test_proto)
test_fec = executable('test_fec', 'tests/test_fec.c', dependencies: proto_dep)
test('fec reed-solomon', test_fec)
test_video = executable('test_video', 'tests/test_video.c', dependencies: proto_dep)
test('video packetiser and reassembler', test_video, timeout: 120)
test_stream = executable('test_stream', 'tests/test_stream.c', dependencies: proto_dep)
test('video stream over a simulated link', test_stream, timeout: 300)

# the daemon and the desktop test client
pdispd = executable('pdispd', 'daemon/pdispd.c', 'daemon/usblink.c', dependencies: [vdisplay_dep, encoder_dep, proto_dep, dependency('libusb-1.0'), dependency('threads')])
libav_deps = [dependency('libavcodec'), dependency('libavutil'), dependency('libswscale')]
pdisp_view = executable('pdisp-view', 'tools/pdisp-view.c', 'tools/png.c', dependencies: [proto_dep, zlib_dep, dependency('threads'), dependency('sdl2')] + libav_deps)

m4_loopback = find_program('tests/m4_loopback_check.sh')
test('m4 loopback through the real pipeline', m4_loopback,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm4-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m4_netem = find_program('tests/m4_netem_check.sh')
test('m4 netem loss jitter and bandwidth caps', m4_netem,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm4-netem-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 400,
  suite: 'integration')

test_timesync = executable('test_timesync', 'tests/test_timesync.c', dependencies: proto_dep)
test('clock offset estimator', test_timesync)

test_usbmux = executable('test_usbmux', 'tests/test_usbmux.c', dependencies: [proto_dep, dependency('threads')])
test('usbmux over a byte pipe', test_usbmux)

test('m4 clock sync with a skewed viewer clock', m4_loopback,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm4-skew-artifacts', '-K', '7654321'],
  env: {'EXPECT_OFFSET_US': '-7654321', 'PDISP_TEST_PORT': '47113'},
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m4_reconfigure = find_program('tests/m4_reconfigure_check.sh')
test('m4 reconfigure on a display change', m4_reconfigure,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm4-reconfigure-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m7_usb = find_program('tests/m7_usb_check.sh')
test('m7 monitor over a USB cable', m7_usb,
  args: [pdispd, pdisp_pattern, meson.current_build_dir() / 'm7-usb-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 240,
  suite: 'integration')

m5_rotate = find_program('tests/m5_rotate_check.sh')
test('m5 rotation follows the phone', m5_rotate,
  args: [pdispd, pdisp_pattern, meson.current_build_dir() / 'm5-rotate-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 180,
  suite: 'integration')

test_crypto = executable('test_crypto', 'tests/test_crypto.c', dependencies: proto_dep)
test('crypto primitives (known answers)', test_crypto)
