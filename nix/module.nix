{
  config,
  lib,
  options,
  pkgs,
  ...
}:
let
  cfg = config.programs.tether;
  bluetoothService = adapter: {
    name = "tether-btclass@${adapter}";
    value = {
      description = "Set Bluetooth Class of Device for Tether on ${adapter}";
      after = [ "bluetooth.service" ];
      partOf = [ "bluetooth.service" ];
      wantedBy = [ "bluetooth.service" ];
      serviceConfig = {
        Type = "oneshot";
        RemainAfterExit = true;
        # btmgmt epolls its stdin before it runs the command,
        # ${adapter} is a hint: a controller that re-enumerates comes back under
        # the next free index, so resolve the adapter on every attempt.
        ExecStart = pkgs.writeShellScript "tether-btclass-${adapter}" ''
          for attempt in $(${pkgs.coreutils}/bin/seq 30); do
            hci=${adapter}
            [ -e /sys/class/bluetooth/$hci ] \
              || hci=$(${pkgs.coreutils}/bin/ls /sys/class/bluetooth 2>/dev/null | ${pkgs.coreutils}/bin/head -n1)
            if [ -n "$hci" ]; then
              echo | ${pkgs.bluez}/bin/btmgmt --index $hci class 4 8 >/dev/null 2>&1
              if echo | ${pkgs.bluez}/bin/btmgmt --index $hci info 2>/dev/null \
                | ${pkgs.gnugrep}/bin/grep -q "class 0x..0408"; then
                exit 0
              fi
            fi
            ${pkgs.coreutils}/bin/sleep 1
          done
          exit 1
        '';
        TimeoutStartSec = 60;
      };
    };
  };
in
with lib;
{
  options.programs.tether = {
    enable = mkEnableOption "Tether, an iPhone integration bridge for Linux";
    package = mkOption {
      type = with types; package;
      default = pkgs.callPackage ./package.nix { };
      defaultText = literalExpression "pkgs.callPackage ./nix/package.nix { }";
      description = "The Tether package to install and integrate.";
    };
    wifi.enable = mkEnableOption "Tether Wi-Fi discovery through Avahi";
    wifi.openFirewall = mkEnableOption "the mDNS and TCP firewall ports required by Tether";
    bluetooth.enable = mkEnableOption "Tether Bluetooth integration";
    bluetooth.adapters = mkOption {
      type = with types; listOf (strMatching "hci[0-9]+");
      default = [ "hci0" ];
      example = [
        "hci0"
        "hci1"
      ];
      description = "Bluetooth HCI adapters whose class Tether should configure.";
    };
    extensions = mkOption {
      type =
        with types;
        listOf (enum [
          "firefox"
          "chromium"
          "thunderbird"
        ]);
      default = [ ];
      defaultText = literalExpression "[ ]";
      description = "The Tether Browser/Mail extensions to retrieve OTP codes";
    };
  };

  config = mkMerge [
    (mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
    })

    (mkIf (cfg.enable && (builtins.elem "firefox" cfg.extensions)) {
      programs.firefox.nativeMessagingHosts.packages = [ cfg.package ];
    })

    (mkIf (cfg.enable && (builtins.elem "chromium" cfg.extensions)) {
      environment.etc = {
        "chromium/native-messaging-hosts/com.tether.extension.json".source =
          "${cfg.package}/etc/chromium/native-messaging-hosts/com.tether.extension.json";
        "opt/chrome/native-messaging-hosts/com.tether.extension.json".source =
          "${cfg.package}/etc/opt/chrome/native-messaging-hosts/com.tether.extension.json";
      };
    })

    (mkIf (cfg.enable && (builtins.elem "thunderbird" cfg.extensions)) {
      programs.thunderbird.package = mkDefault (
        pkgs.thunderbird.override {
          nativeMessagingHosts = [ cfg.package ];
        }
      );
    })

    (mkIf (cfg.enable && cfg.wifi.enable) {
      services.avahi = {
        enable = true;
        openFirewall = mkDefault false;
      }
      # nixpkgs replaced publish.* with a freeform `settings` and warns on the
      # old names; keep them for releases that predate `settings`.
      // (
        if options.services.avahi ? settings then
          {
            settings.publish = {
              disable-publishing = false;
              disable-user-service-publishing = false;
              publish-addresses = true;
            };
          }
        else
          {
            publish = {
              enable = true;
              userServices = true;
            };
          }
      );
    })

    (mkIf (cfg.enable && cfg.wifi.openFirewall) {
      services.avahi.openFirewall = true;
      networking.firewall.allowedTCPPorts = [ 5134 ];
    })

    (mkIf (cfg.enable && cfg.bluetooth.enable) {
      hardware.bluetooth.enable = true;
      hardware.bluetooth.settings.General.Experimental = true;
      systemd.services = builtins.listToAttrs (map bluetoothService cfg.bluetooth.adapters);
    })
  ];
}
