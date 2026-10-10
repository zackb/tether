{
  description = "Use an iPhone with Linux";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      tetherModule = import ./nix/module.nix;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          tether = pkgs.callPackage ./nix/package.nix {
            npmConfigHook = pkgs.npmHooks.npmConfigHook;
          };
        in
        {
          default = tether;
          inherit tether;
        }
      );

      apps = forAllSystems (
        system:
        let
          tether = self.packages.${system}.tether;
          app = name: {
            type = "app";
            program = "${tether}/bin/${name}";

            meta = {
              description = "Bridge an iPhone to the Linux desktop: clipboard, files, messages, and notifications";
              maintainers = [ { email = "zack@bartel.com"; } ];
              homepage = "https://github.com/zackb/tether";
              license = nixpkgs.lib.licenses.mit;
              platforms = systems;
            };
          };
        in
        {
          default = app "tether-gtk";
          tether = app "tether";
          tether-gtk = app "tether-gtk";
          tether-tui = app "tether-tui";
          tetherd = app "tetherd";
          tether-dialog = app "tether-dialog";
        }
      );

      overlays.default = final: _prev: {
        tether = final.callPackage ./nix/package.nix {
          npmConfigHook = final.npmHooks.npmConfigHook;
        };
      };

      nixosModules.default = self.nixosModules.tether;
      nixosModules.tether = tetherModule;

      checks = forAllSystems (system: {
        package = self.packages.${system}.tether;
        module = import ./nix/module-test.nix {
          inherit nixpkgs system tetherModule;
        };
      });

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt);

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.tether ];
            packages = [ pkgs.clang-tools ];
          };
        }
      );
    };
}
