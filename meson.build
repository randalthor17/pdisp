project('pdisp', 'c',
  version: '0.0.1',
  license: 'MIT',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true', 'b_ndebug=false',
                    'buildtype=debugoptimized'])

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

# the GUI is optional and the only C++
have_cpp = add_languages('cpp', required: get_option('gui'), native: false)
qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets', 'Qml', 'Quick', 'QuickControls2', 'Network'], required: get_option('gui'))

m_dep = meson.get_compiler('c').find_library('m')
evdi_dep = dependency('evdi')

inc = include_directories('daemon', 'tools', 'proto')

edid_lib = static_library('pdisp-edid', 'daemon/edid.c',
  include_directories: inc,
  dependencies: m_dep)
edid_dep = declare_dependency(link_with: edid_lib, include_directories: inc)

vdisplay_lib = static_library('pdisp-vdisplay', 'daemon/vdisplay.c', 'daemon/ddcci.c',
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

encoder_lib = static_library('pdisp-encoder', 'daemon/encoder.c', 'daemon/hevc_headers.c', 'daemon/h264_headers.c',
  include_directories: inc,
  dependencies: [libva_dep, libva_drm_dep])
encoder_dep = declare_dependency(
  link_with: encoder_lib, include_directories: inc, dependencies: [libva_dep, libva_drm_dep])

mc_inc = include_directories('third_party/monocypher')
monocypher = static_library('monocypher', 'third_party/monocypher/monocypher.c', c_args: ['-w'])
proto_lib = static_library('pdisp-proto', 'proto/proto.c', 'proto/fec.c', 'proto/video.c', 'proto/vsend.c', 'proto/vrecv.c', 'proto/net.c', 'proto/timesync.c', 'proto/usbmux.c', 'proto/crypto.c', 'proto/noise.c', 'proto/identity.c', 'proto/secure.c', 'proto/handshake.c', 'proto/inputseq.c', 'proto/palm.c', include_directories: [inc, mc_inc], link_with: monocypher)
proto_dep = declare_dependency(link_with: proto_lib, include_directories: inc)

# The development and diagnostic tools are one program with subcommands (pdisp-dev edid|vdisplay|capture|encode|evdump|touch-demo). The tests use links
# named after the tool: the program looks at the name it was started under.
pdisp_dev = executable('pdisp-dev', 'tools/pdisp-dev.c', 'tools/pdisp-edid.c', 'tools/pdisp-vdisplay.c', 'tools/pdisp-capture.c', 'tools/png.c',
  'tools/pdisp-encode.c', 'tools/pdisp-evdump.c', 'tools/pdisp-touch-demo.c', 'daemon/touch.c',
  dependencies: [edid_cli_dep, vdisplay_dep, encoder_dep, proto_dep, zlib_dep])
dev_links = {}
foreach t : ['edid', 'vdisplay', 'capture', 'encode', 'evdump', 'touch-demo']
  dev_links += { t: custom_target('pdisp-' + t, output: 'pdisp-' + t, command: [find_program('ln'), '-sf', 'pdisp-dev', '@OUTPUT@'], depends: pdisp_dev, build_by_default: true) }
endforeach
pdisp_edid = dev_links['edid'].full_path()
pdisp_vdisplay = dev_links['vdisplay'].full_path()
pdisp_capture = dev_links['capture'].full_path()
pdisp_encode = dev_links['encode'].full_path()
pdisp_pattern = executable('pdisp-pattern', 'tools/pdisp-pattern.c', xdg_shell_h, xdg_shell_c,
  include_directories: inc, dependencies: wayland_client_dep)



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

test_h264_headers = executable('test_h264_headers', 'tests/test_h264_headers.c', 'daemon/h264_headers.c',
  include_directories: inc)
test('h264 headers unit', test_h264_headers)

# synthetic-frame encoder driver used by the loss recovery test
enc_synth = executable('enc_synth', 'tests/enc_synth.c', dependencies: [encoder_dep], include_directories: inc)

test('loss recovery', find_program('tests/loss_recovery_check.sh'),
  args: [enc_synth, meson.current_build_dir() / 'loss-artifacts', 'hevc'],
  protocol: 'exitcode',
  timeout: 120,
  suite: 'integration')
test('loss recovery (h264)', find_program('tests/loss_recovery_check.sh'),
  args: [enc_synth, meson.current_build_dir() / 'loss-artifacts-h264', 'h264'],
  protocol: 'exitcode',
  timeout: 120,
  suite: 'integration')

test_proto = executable('test_proto', 'tests/test_proto.c', dependencies: proto_dep)
test('proto wire format', test_proto)
test_fec = executable('test_fec', 'tests/test_fec.c', dependencies: proto_dep)
test('fec reed-solomon', test_fec)
test_video = executable('test_video', 'tests/test_video.c', dependencies: proto_dep)
test('video packetiser and reassembler', test_video, timeout: 120)
test_stream = executable('test_stream', 'tests/test_stream.c', dependencies: proto_dep)
test('video stream over a simulated link', test_stream, timeout: 300)

# the daemon and the desktop test client
pdispd = executable('pdispd', 'daemon/pdispd.c', 'daemon/usblink.c', 'daemon/touch.c', 'daemon/control.c', 'daemon/json.c', 'daemon/mdns.c', 'daemon/ratestep.c', dependencies: [vdisplay_dep, encoder_dep, proto_dep, dependency('libusb-1.0'), dependency('threads')], install: true)
libav_deps = [dependency('libavcodec'), dependency('libavutil'), dependency('libswscale')]
pdisp_view = executable('pdisp-view', 'tools/pdisp-view.c', 'tools/png.c', dependencies: [proto_dep, zlib_dep, dependency('threads'), dependency('sdl2'), meson.get_compiler('c').find_library('m')] + libav_deps)

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

test_noise = executable('test_noise', 'tests/test_noise.c', dependencies: proto_dep)
test('noise xx handshake', test_noise)

test_identity = executable('test_identity', 'tests/test_identity.c', dependencies: proto_dep)
test('identity files and key stores', test_identity)

test_secure = executable('test_secure', 'tests/test_secure.c', dependencies: [proto_dep, dependency('threads')])
test('secure channels', test_secure)

test_handshake = executable('test_handshake', 'tests/test_handshake.c', dependencies: [proto_dep, dependency('threads')])
test('handshake driver', test_handshake)

# the command line: control socket, keys, brightness and the tray application as subcommands
pdisp_ctl = executable('pdisp', 'tools/pdisp.c', 'tools/pdisp-ctl.c', 'tools/pdisp-keys.c', 'tools/pdisp-brightness.c', dependencies: proto_dep, install: true)
m6_pairing = find_program('tests/m6_pairing_check.sh')
test('m6 pairing and authorisation', m6_pairing,
  args: [pdispd, pdisp_view, meson.current_build_dir() / 'm6-pairing-artifacts', pdisp_ctl],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 300,
  suite: 'integration')


test_inputseq = executable('test_inputseq', 'tests/test_inputseq.c', dependencies: proto_dep)
test('input event stream', test_inputseq)

test_touch = executable('test_touch', 'tests/test_touch.c', 'daemon/touch.c', dependencies: proto_dep)
test('uinput touchscreen', test_touch)


m8_touch = find_program('tests/m8_touch_check.sh')
test('m8 touch reaches the virtual output', m8_touch,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm8-touch-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 240,
  suite: 'integration')


test_inputdev = executable('test_inputdev', 'tests/test_inputdev.c', 'daemon/touch.c', dependencies: proto_dep)
test('touchscreen and pen devices', test_inputdev)

test_palm = executable('test_palm', 'tests/test_palm.c', dependencies: proto_dep)
test('palm rejection', test_palm)

m8_gesture = find_program('tests/m8_gesture_check.sh')
test('m8 touch gestures in a real application', m8_gesture,
  args: [pdispd, pdisp_view, meson.current_build_dir() / 'm8-gesture-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 240,
  suite: 'integration')

m9_lifecycle = find_program('tests/m9_lifecycle_check.sh')
test('m9 virtual monitor lifecycle', m9_lifecycle,
  args: [pdispd, pdisp_view, meson.current_build_dir() / 'm9-lifecycle-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 400,
  suite: 'integration')

# opens a window on your desktop (the compositor moves it around): skips unless PDISP_TEST_WINDOWS=1
m9_windows = find_program('tests/m9_windows_check.sh')
test('m9 windows follow the virtual monitor', m9_windows,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm9-windows-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 400,
  suite: 'integration')

m10_text = find_program('tests/m10_text_check.sh')
test('m10 text quality of a static screen', m10_text,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm10-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m10_burst = find_program('tests/m10_burst_latency.sh')
test('m10 latency of a change right after refinement', m10_burst,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm10-burst-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

m10_real = find_program('tests/m10_real_text_check.sh')
test('m10 real text (opt-in: PDISP_TEST_WINDOWS=1)', m10_real,
  args: [pdispd, pdisp_view, pdisp_pattern, meson.current_build_dir() / 'm10-real-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')

# ---- installation (nix/pdisp.nix, docs/install.md)
install_data('contrib/pdisp-pair-prompt', install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')
install_data('contrib/udev/70-pdisp.rules', install_dir: get_option('prefix') / 'lib/udev/rules.d')
install_data('contrib/niri/pdisp.kdl', install_dir: get_option('datadir') / 'pdisp')
configure_file(input: 'contrib/systemd/pdispd.service.in', output: 'pdispd.service',
  configuration: { 'BINDIR': get_option('prefix') / get_option('bindir') },
  install_dir: get_option('prefix') / 'lib/systemd/user')

json_lib = static_library('pdisp-json', 'daemon/json.c', include_directories: inc)
json_dep = declare_dependency(link_with: json_lib, include_directories: inc)
test_json = executable('test_json', 'tests/test_json.c', dependencies: json_dep)
test_ratestep = executable('test_ratestep', 'tests/test_ratestep.c', 'daemon/ratestep.c')
test('rate governor', test_ratestep)
test_mdns = executable('test_mdns', 'tests/test_mdns.c', 'daemon/mdns.c', dependencies: dependency('threads'))
test('mdns packets', test_mdns)
test_ddcci = executable('test_ddcci', 'tests/test_ddcci.c', 'daemon/ddcci.c')
test('ddc/ci monitor', test_ddcci)
test('json helper', test_json)

m11_control = find_program('tests/m11_control_check.sh')
test('m11 control socket', m11_control,
  args: [pdispd, pdisp_view, pdisp_ctl, meson.current_build_dir() / 'm11-control-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 200,
  suite: 'integration')

m12_rate = find_program('tests/m12_rate_step_check.sh')
test('m12 stable refresh rate', m12_rate,
  args: [pdispd, pdisp_view, pdisp_pattern, pdisp_ctl, meson.current_build_dir() / 'm12-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 240,
  suite: 'integration')

if qt6_dep.found() and have_cpp
  subdir('gui')
endif

m13_brightness = find_program('tests/m13_brightness_check.sh')
test('m13 brightness over DDC/CI', m13_brightness,
  args: [pdispd, pdisp_view, pdisp_ctl, meson.current_build_dir() / 'm13-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 90,
  suite: 'integration')
