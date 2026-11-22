# The pdisp daemon and its tools, built from this repository with meson.
{ stdenv, lib, meson, ninja, pkg-config, wayland-scanner, libevdi, libdrm, libva, libusb1, wayland, wayland-protocols, zlib, ffmpeg-headless, SDL2 }:

stdenv.mkDerivation {
  pname = "pdisp";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../meson.build
      ../daemon
      ../proto
      ../tools
      ../tests
      ../third_party
      ../contrib
    ];
  };

  nativeBuildInputs = [ meson ninja pkg-config wayland-scanner ];
  buildInputs = [ libevdi libdrm libva libusb1 wayland wayland-protocols zlib ffmpeg-headless SDL2 ];

  # the integration tests need niri, a GPU and a device; `meson test` in the development shell runs them
  doCheck = false;

  meta = {
    description = "Use an Android phone or tablet as a Linux second monitor";
    license = lib.licenses.gpl3Plus;
    platforms = [ "x86_64-linux" ];
    mainProgram = "pdispd";
  };
}
