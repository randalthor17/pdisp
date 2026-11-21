project('pdisp', 'c',
  version: '0.0.1',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=true', 'b_ndebug=false',
                    'buildtype=debugoptimized'])

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

m_dep = meson.get_compiler('c').find_library('m')

edid_lib = static_library('pdisp-edid', 'daemon/edid.c',
  include_directories: include_directories('daemon'),
  dependencies: m_dep)

edid_dep = declare_dependency(
  link_with: edid_lib,
  include_directories: include_directories('daemon'))

pdisp_edid = executable('pdisp-edid', 'tools/pdisp-edid.c', dependencies: edid_dep)

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
