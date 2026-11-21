{
  description = "pdisp: use an Android device as a Linux second monitor";

  inputs.nixpkgs.url = "flake:nixpkgs";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      libevdi = pkgs.callPackage ./nix/libevdi.nix { evdi = pkgs.linuxPackages.evdi; };
    in
    {
      packages.${system} = { inherit libevdi; };

      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          gcc
          gdb
          meson
          ninja
          pkg-config
          wayland-scanner
          edid-decode
          jq
          iproute2
          ffmpeg-headless
        ];
        buildInputs = with pkgs; [
          libevdi
          libdrm
          libva
          libusb1
          wayland
          wayland-protocols
          zlib
          ffmpeg-headless
        ];
      };
    };
}
