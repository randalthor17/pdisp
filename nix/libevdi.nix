# libevdi from the evdi source that nixpkgs already ships. nixpkgs' `evdi` package installs the
# library but no header and no pkg-config file, so we build the userspace library ourselves.
{ stdenv, lib, evdi, libdrm, pkg-config }:

stdenv.mkDerivation {
  pname = "libevdi";
  inherit (evdi) version src;

  sourceRoot = "source/library";

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libdrm ];

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  postInstall = ''
    install -Dm644 evdi_lib.h $out/include/evdi_lib.h
    mkdir -p $out/lib/pkgconfig
    cat > $out/lib/pkgconfig/evdi.pc <<EOF
    prefix=$out
    libdir=$out/lib
    includedir=$out/include

    Name: evdi
    Description: Extensible Virtual Display Interface userspace library
    Version: ${evdi.version}
    Libs: -L\''${libdir} -levdi
    Cflags: -I\''${includedir}
    EOF
  '';

  meta = {
    description = "Userspace library for the evdi virtual display kernel module";
    license = lib.licenses.lgpl21Only;
    platforms = lib.platforms.linux;
  };
}
