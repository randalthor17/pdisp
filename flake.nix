{
  description = "pdisp: use an Android device as a Linux second monitor";

  inputs.nixpkgs.url = "flake:nixpkgs";

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
    in
    {
      packages.${system} = { inherit libevdi; };

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
          iproute2
          ffmpeg-headless
          SDL2
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
        ];
      };
      };
    };
}
