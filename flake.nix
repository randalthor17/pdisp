{
  description = "pdisp: use an Android device as a Linux second monitor";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # The Android toolchain is unfree (SDK licences), so it lives in its own shell: `nix develop .#android`
      apkgs = import nixpkgs { inherit system; config = { allowUnfree = true; android_sdk.accept_license = true; }; };
      androidsdk = (apkgs.androidenv.composeAndroidPackages {
        platformVersions = [ "36" ];
        buildToolsVersions = [ "36.0.0" ];
        includeNDK = true;
        ndkVersions = [ "27.2.12479018" ];
        cmakeVersions = [ "3.22.1" ];
      }).androidsdk;
      libevdi = pkgs.callPackage ./nix/libevdi.nix { evdi = pkgs.linuxPackages.evdi; };
      pdisp = pkgs.callPackage ./nix/pdisp.nix { inherit libevdi; };
      pdisp-full = pkgs.callPackage ./nix/pdisp.nix { inherit libevdi; withGui = true; };
    in
    {
      packages.${system} = { inherit libevdi pdisp pdisp-full; default = pdisp; };

      nixosModules.default = import ./nix/nixos-module.nix { inherit self; };
      homeManagerModules.default = import ./nix/hm-module.nix { inherit self; };

      # evaluates a minimal system with the module switched on (no build): `nix flake check`
      checks.${system} = {
        vm = import ./nix/vm-test.nix { inherit pkgs self; };
        nixos-module = (nixpkgs.lib.nixosSystem {
        inherit system;
        modules = [
          self.nixosModules.default
          { services.pdisp = { enable = true; openFirewall = true; };
            boot.loader.grub.enable = false; fileSystems."/" = { device = "none"; fsType = "tmpfs"; }; system.stateVersion = "25.11"; }
        ];
      }).config.system.build.toplevel;
      };

      devShells.${system} = {
      android = apkgs.mkShell {
        packages = [ apkgs.jdk17 apkgs.gradle androidsdk apkgs.meson apkgs.ninja ];
        ANDROID_HOME = "${androidsdk}/libexec/android-sdk";
        ANDROID_SDK_ROOT = "${androidsdk}/libexec/android-sdk";
        ANDROID_NDK_ROOT = "${androidsdk}/libexec/android-sdk/ndk-bundle";
        JAVA_HOME = "${apkgs.jdk17}";
      };

      default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          gcc
          gdb
          meson
          ninja
          pkg-config
          wayland-scanner
          edid-decode
          jq
          imagemagick # the gesture and window checks
          loupe
          android-tools # adb, for the checks that need a tablet on a USB cable (m5, m7)
          ddcutil # the brightness check (m13) talks DDC/CI to the virtual monitor
          iproute2
          ffmpeg-headless
          SDL2
          qt6.wrapQtAppsHook
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
          SDL2
          qt6.qtbase
          qt6.qtdeclarative
          qt6.qtwayland
          qt6.qtsvg
        ];

        # the user's profile has Qt 5 QML modules: point Qt at its own 6 ones, and only those
        shellHook = ''
          export QML_IMPORT_PATH=${pkgs.qt6.qtdeclarative}/lib/qt-6/qml
          export QML2_IMPORT_PATH=$QML_IMPORT_PATH
          export QT_PLUGIN_PATH=${pkgs.qt6.qtbase}/lib/qt-6/plugins:${pkgs.qt6.qtwayland}/lib/qt-6/plugins:${pkgs.qt6.qtsvg}/lib/qt-6/plugins
        '';
      };
      };
    };
}
