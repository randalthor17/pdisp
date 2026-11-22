# The pdisp daemon and its tools, built from this repository with meson. With withGui the Qt 6 / QML application pdisp-gui is built too.
{ stdenv, lib, meson, ninja, pkg-config, wayland-scanner, libevdi, libdrm, libva, libusb1, wayland, wayland-protocols, zlib, ffmpeg-headless, SDL2
, qt6, material-symbols, withGui ? false }:

stdenv.mkDerivation {
  pname = if withGui then "pdisp-full" else "pdisp";
  version = "0.2.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../meson_options.txt
      ../daemon
      ../proto
      ../tools
      ../tests
      ../third_party
      ../contrib
      ../gui
    ];
  };

  nativeBuildInputs = [ meson ninja pkg-config wayland-scanner ] ++ lib.optional withGui qt6.wrapQtAppsHook;
  buildInputs = [ libevdi libdrm libva libusb1 wayland wayland-protocols zlib ffmpeg-headless SDL2 ]
    ++ lib.optionals withGui [ qt6.qtbase qt6.qtdeclarative qt6.qtwayland qt6.qtsvg ];

  mesonFlags = [ (lib.mesonEnable "gui" withGui) ]
    # the icon font the GUI loads when the system has none
    ++ lib.optional withGui "-Dsymbols_font=${material-symbols}/share/fonts/truetype/MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf";

  # only the GUI is a Qt program: do not wrap the daemon and the tools
  dontWrapQtApps = true;
  postFixup = lib.optionalString withGui ''
    wrapQtApp $out/libexec/pdisp/pdisp-gui
  '';

  # the integration tests need niri, a GPU and a device; `meson test` in the development shell runs them
  doCheck = false;

  meta = {
    description = "Use an Android phone or tablet as a Linux second monitor";
    license = lib.licenses.gpl3Plus;
    platforms = [ "x86_64-linux" ];
    mainProgram = "pdispd";
  };
}
