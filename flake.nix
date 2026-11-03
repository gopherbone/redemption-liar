# SPDX-License-Identifier: GPL-2.0-or-later
{
  description = "Redemption: Liar on Mortar: a loader for your own retail GP2X Wiz copy";

  # To use a local checkout of Mortar (or until Mortar's layered flake is on
  # GitHub), add: --override-input mortar path:../mortar
  inputs.mortar.url = "github:gopherbone/mortar";

  outputs = { self, mortar }:
    let
      nixpkgs = mortar.inputs.nixpkgs;
      lib = nixpkgs.lib;
      # The loader's licence is marked unfree on purpose (see package/default.nix).
      pkgsFor = system: import nixpkgs {
        inherit system;
        config.allowUnfreePredicate = pkg: lib.getName pkg == "liar";
      };
      target = pkgsFor "aarch64-linux";
      loader = target.callPackage ./package { };
    in {
      # Import next to Mortar's core: mortar.lib.mkSystem { ports = [ liar.nixosModules.default ]; }
      nixosModules.default = ./module.nix;

      packages.aarch64-linux = {
        liar = loader;
        default = loader;
      };

      # nix run .#install -- --data PATH --host HOST
      apps = lib.genAttrs [ "aarch64-darwin" "x86_64-darwin" "x86_64-linux" "aarch64-linux" ] (system:
        let
          installer = (pkgsFor system).callPackage ./installer { inherit loader; };
        in {
          install = { type = "app"; program = "${installer}/bin/liar-install"; };
        });

      # Evaluates and builds with Mortar's interfaces.
      nixosConfigurations.check = mortar.lib.mkSystem { ports = [ self.nixosModules.default ]; };
    };
}
