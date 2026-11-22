# A NixOS VM with the module switched on: what one rebuild gives a machine, as far as a VM without a GPU or a phone can show.
{ pkgs, self }:
pkgs.testers.runNixOSTest {
  name = "pdisp-module";
  nodes.machine = { pkgs, ... }: {
    imports = [ self.nixosModules.default ];
    services.pdisp = { enable = true; openFirewall = true; extraArgs = [ "-g" "7" ]; };
    users.users.alice = { isNormalUser = true; uid = 1000; };
    services.getty.autologinUser = "alice";
    environment.systemPackages = [ pkgs.iproute2 pkgs.nftables ];
  };
  testScript = ''
    machine.wait_for_unit("multi-user.target")

    def user(cmd):
        return machine.succeed("su - alice -c 'XDG_RUNTIME_DIR=/run/user/1000 systemctl --user " + cmd + "'")

    with subtest("the evdi module is loaded with a device already created"):
        machine.succeed("lsmod | grep -q '^evdi '")
        machine.succeed("test \"$(cat /sys/devices/evdi/count)\" = 1")

    with subtest("the udev rule is installed, the tools are on the path"):
        machine.succeed("grep -rq 'ATTR{idVendor}==\"04e8\"' /etc/udev/rules.d/")  # Samsung, from 70-pdisp.rules
        machine.succeed("which pdispd pdisp-keys pdisp-pair-prompt notify-send")

    with subtest("the firewall lets the device in on the port"):
        out = machine.succeed("nft list ruleset")
        assert out.count("47110") >= 2, out  # tcp and udp

    with subtest("the user service starts and listens, with the options from the configuration"):
        machine.succeed("loginctl enable-linger alice")
        machine.wait_for_unit("user@1000.service")
        out = user("cat pdispd")
        assert "-P ask" in out and "pdisp-pair-prompt" in out and "-g 7" in out and "-u -W" in out, out
        user("start pdispd")
        machine.wait_until_succeeds("ss -ltn | grep -q 47110")
        user("is-active pdispd")
  '';
}
