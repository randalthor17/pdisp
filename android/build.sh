#!/bin/sh
# Builds the app inside the Android dev shell: android/build.sh [gradle args], default assembleDebug.
# aapt2 from Maven is a generic-Linux binary that does not run on NixOS, so the SDK's own aapt2 is used.
cd "$(dirname "$0")" || exit 1
exec nix develop ..#android -c sh -c '
  gradle --no-daemon -Pandroid.aapt2FromMavenOverride="$ANDROID_HOME/build-tools/36.0.0/aapt2" "$@"' sh "${@:-assembleDebug}"
