# pdisp-gui: Qt 6 / QML. Built when Qt 6 with Quick Controls is found (-Dgui=disabled turns it off).
qt6 = import('qt6')

gui_moc = qt6.compile_moc(headers: 'backend.h', dependencies: qt6_dep)
gui_res = qt6.compile_resources(name: 'pdisp-gui-resources', sources: 'pdisp.qrc')

pdisp_gui = executable('pdisp-gui', 'main.cpp', 'backend.cpp', gui_moc, gui_res,
  dependencies: qt6_dep,
  override_options: ['werror=false', 'warning_level=1', 'cpp_std=c++17'],
  cpp_args: get_option('symbols_font') != '' ? ['-DPDISP_SYMBOLS_FONT="' + get_option('symbols_font') + '"'] : [],
  install: true, install_tag: 'gui')

install_data('pdisp-gui.desktop', install_dir: get_option('datadir') / 'applications', install_tag: 'gui')
install_data('pdisp.svg', rename: 'pdisp-gui.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps', install_tag: 'gui')

test('m11 gui against a real daemon', find_program('../tests/m11_gui_check.sh'),
  args: [pdispd, pdisp_view, pdisp_gui, meson.project_build_root() / 'm11-gui-artifacts'],
  protocol: 'exitcode',
  is_parallel: false,
  timeout: 120,
  suite: 'integration')
