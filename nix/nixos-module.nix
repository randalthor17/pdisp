# NixOS module: the evdi kernel module with a virtual device, the udev rule for USB, the firewall port, and the daemon as a user service.
{ self }:
{ config, lib, pkgs, ... }:
let
  cfg = config.services.pdisp;
  args = [ "-p" (toString cfg.port) "-P" "ask" "-A" "${cfg.package}/bin/pdisp-pair-prompt" ]
    ++ lib.optionals cfg.usb [ "-u" "-W" ] # a cable and the network at the same time
    ++ cfg.extraArgs;
in
{
  options.services.pdisp = {
    enable = lib.mkEnableOption "pdisp, an Android phone or tablet as a second monitor";

    package = lib.mkOption {
      type = lib.types.package;
      default = if cfg.gui.enable then self.packages.${pkgs.stdenv.hostPlatform.system}.pdisp-full else self.packages.${pkgs.stdenv.hostPlatform.system}.pdisp;
      defaultText = lib.literalExpression "pdisp.packages.\${system}.pdisp";
      description = "The pdisp package.";
    };

    gui.enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Also install and start pdisp-gui, the tray application for status, paired devices, settings and approving new devices (it pulls in Qt 6).";
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 47110;
      description = "TCP and UDP port the daemon listens on for devices on the network.";
    };

    openFirewall = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Open the port for the network connection (Wi-Fi). Off by default: nothing is needed for a USB cable. The port is open on every interface;
        devices still have to be paired (a notification asks), and everything on the wire is encrypted.
      '';
    };

    usb = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Also look for a USB cable: the daemon asks the device to switch to accessory mode and streams over it. The network stays on (pdispd -u -W).";
    };

    evdiDevices = lib.mkOption {
      type = lib.types.ints.positive;
      default = 1;
      description = "How many virtual display devices the evdi module creates when it loads (one per device that can be connected at once).";
    };

    extraArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [ "-R" "90" "-b" "20000" ];
      description = "More options for pdispd (see `pdispd -h`): refresh-rate cap, start bitrate, grace period.";
    };
  };

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [ config.boot.kernelPackages.evdi ];
    boot.kernelModules = [ "evdi" ];
    # a device that exists from the start: adding one later would need root
    boot.extraModprobeConfig = "options evdi initial_device_count=${toString cfg.evdiDevices}";

    services.udev.packages = [ cfg.package ]; # lib/udev/rules.d/70-pdisp.rules: the user may open the Android devices (uaccess)
    environment.systemPackages = [ cfg.package pkgs.libnotify ]; # pdisp-keys, and notify-send for the pairing prompt

    networking.firewall = lib.mkIf cfg.openFirewall {
      allowedTCPPorts = [ cfg.port ];
      allowedUDPPorts = [ cfg.port 5353 ]; # 5353: mDNS, so the app can find this PC (queries from the LAN come in on it)
    };

    systemd.user.services.pdisp-gui = lib.mkIf cfg.gui.enable {
      description = "pdisp tray application";
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      after = [ "graphical-session.target" "pdispd.service" ];
      serviceConfig = {
        ExecStart = "${cfg.package}/bin/pdisp-gui --background";
        Restart = "on-failure";
        RestartSec = 3;
      };
    };

    systemd.user.services.pdispd = {
      description = "pdisp second monitor daemon";
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      after = [ "graphical-session.target" ];
      path = [ pkgs.libnotify ];
      serviceConfig = {
        ExecStart = "${cfg.package}/bin/pdispd ${lib.escapeShellArgs args}";
        Restart = "on-failure";
        RestartSec = 3;
      };
    };
  };
}
