# home-manager module: the niri input mapping that keeps the device's touch and pen on its own monitor (docs/touch.md).
# With home-manager's niri module (wayland.windowManager.niri) it adds the mapping to `settings.input` itself; without it, it writes
# ~/.config/niri/pdisp.kdl, which the niri configuration includes with the line `include "pdisp.kdl"`.
{ self }:
{ config, lib, options, pkgs, ... }:
let
  cfg = config.programs.pdisp;
  hasNiriModule = options ? wayland && options.wayland ? windowManager && options.wayland.windowManager ? niri;
in
{
  options.programs.pdisp = {
    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.pdisp;
      defaultText = lib.literalExpression "pdisp.packages.\${system}.pdisp";
      description = "The pdisp package (the niri file comes from it).";
    };

    niri.enable = lib.mkEnableOption "the niri mapping of the pdisp monitor's touch screen and pen tablet";

    niri.output = lib.mkOption {
      type = lib.types.str;
      default = "PNP(PDI) pdisp PDISP0001";
      description = ''
        The monitor's identity (make, model, serial) as niri names it. With the daemon's defaults (`-n pdisp`, serial PDISP0001) it is the
        default; it does not depend on the connector name.
      '';
    };

    niri.include = lib.mkOption {
      type = lib.types.str;
      default = ''include "pdisp.kdl"'';
      readOnly = true;
      description = "Without home-manager's niri module: the line that pulls ~/.config/niri/pdisp.kdl into a niri configuration.";
    };
  };

  config = lib.mkIf cfg.niri.enable (lib.mkMerge [
    (lib.optionalAttrs hasNiriModule {
      wayland.windowManager.niri.settings.input = {
        touch.map-to-output = cfg.niri.output;
        tablet.map-to-output = cfg.niri.output;
      };
    })
    (lib.optionalAttrs (!hasNiriModule) {
      xdg.configFile."niri/pdisp.kdl".source = "${cfg.package}/share/pdisp/pdisp.kdl";
    })
  ]);
}
