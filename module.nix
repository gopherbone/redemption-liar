# SPDX-License-Identifier: GPL-2.0-or-later
# Redemption: Liar (retail GP2X Wiz RPG) as a Mortar port: its own profile,
# /nix/var/nix/profiles/liar, and the user's own files from their card in
# ~/.local/share/liar, both put there by the installer (`nix run .#install`).
# The entry hides until they are there. Needs Mortar's core module: this sets
# mortar.greeter.sessions, mortar.shell.categories and mortar.unfree, and reads
# mortar.user.home.
{ config, lib, pkgs, ... }:

let
  cfg = config.mortar.apps.liar;
in
{
  options.mortar.apps.liar = {
    enable = lib.mkEnableOption "the Redemption: Liar entry, shown once the installer has put the loader and your files from the card on the device" // { default = true; };
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package { };
      defaultText = lib.literalExpression "pkgs.callPackage ./package { }";
      description = ''
        The loader (no game data, no Wiz firmware). Not part of the system:
        the installer builds this (flake output
        `packages.aarch64-linux.liar`) and installs it into `profile`.
      '';
    };
    profile = lib.mkOption {
      type = lib.types.str;
      default = "/nix/var/nix/profiles/liar";
      description = "The Nix profile the loader is installed into (a GC root of its own, untouched by system updates).";
    };
    bin = lib.mkOption {
      type = lib.types.str;
      readOnly = true;
      default = "${cfg.profile}/bin/liar";
      description = "The launcher inside the profile.";
    };
    data = lib.mkOption {
      type = lib.types.str;
      default = "${config.mortar.user.home}/.local/share/liar/liar_e.gpe";
      defaultText = lib.literalExpression ''"''${config.mortar.user.home}/.local/share/liar/liar_e.gpe"'';
      description = "The file the entry waits for (`requires`); the installer puts it here.";
    };
  };

  config = lib.mkMerge [
    { mortar.unfree = [ "liar" ]; }
    (lib.mkIf cfg.enable {
      mortar.shell.categories.liar = "games";
      mortar.greeter.sessions.liar = {
        name = "Redemption: Liar";
        description = "GP2X Wiz · your own copy";
        icon = "pad";
        order = 28;
        requires = cfg.data;
        # No mortar-sdl-run: a 32-bit process, no GPU, its own Wayland client.
        command = "exec ${cfg.bin}";
      };
    })
  ];
}
