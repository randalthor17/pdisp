# home-manager module: the niri input mapping that keeps the device's touch and pen on its own monitor (docs/touch.md).
{ self }:
{ config, lib, pkgs, ... }:
let cfg = config.programs.pdisp; in
{
  options.programs.pdisp = {
    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.pdisp;
      defaultText = lib.literalExpression "pdisp.packages.\${system}.pdisp";
      description = "The pdisp package (the niri file comes from it).";
    };

    niri.enable = lib.mkEnableOption ''
      ~/.config/niri/pdisp.kdl with the touch and tablet mappings for the pdisp monitor. niri does not read it by itself: add the line
      `include "pdisp.kdl"` to your niri configuration (it is `programs.pdisp.niri.include`, a string you can splice into the text you generate)
    '';

    niri.include = lib.mkOption {
      type = lib.types.str;
      default = ''include "pdisp.kdl"'';
      readOnly = true;
      description = "The line that pulls the mapping into a niri configuration.";
    };
  };

  config = lib.mkIf cfg.niri.enable {
    xdg.configFile."niri/pdisp.kdl".source = "${cfg.package}/share/pdisp/pdisp.kdl";
  };
}
