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
        packages = with pkgs; [
          gcc
          gdb
          meson
          ninja
          pkg-config
          edid-decode
          jq
          libevdi
          libdrm
          libva
          libusb1
        ];
      };
    };
}
