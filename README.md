# Redemption: Liar on Mortar

Runs your own retail copy of **Redemption: Liar** (GP2X Wiz, English build,
`Liar_e`) on a TrimUI Brick running [Mortar](https://github.com/gopherbone/mortar).
This repository holds a loader and a Mortar module. It contains **no game data and
no Wiz firmware**, and neither ever goes in a repository, a Nix store or a cache.

## What it is

`liar-run` maps your `liar_e.gpe` (a 32-bit old-ABI ARM executable) and runs it
natively in AArch32, with its imports resolved to `package/src/`: libc wrappers
(`main.c`), the SDL 1.2 it uses (`sdl.c`, `image.c`), a Wayland client and the pad
(`display.c`), sound (`audio.c`) and **a stand-in for the game's INKA DRM libraries
(`drm.c`)**. It is a static armv7 binary: the Brick's graphics and sound libraries
are 64-bit, so it talks to the compositor over the Wayland socket and pipes sound
into the system's 64-bit `pw-cat`.

The loader sources are GPL-2.0-or-later (`LICENSE`), but the package is marked
unfree so it stays out of CI and binary caches: it only exists to run a commercial
game. The game itself remains its publisher's.

## Install

You need your own copy (the card or its `Liar_e` folder), and a Brick you can reach
over SSH as root:

    nix run .#install -- --data PATH --host HOST

This checks your files, builds the loader (aarch64-linux, so on a Mac through a
Linux builder), installs it into its own profile on the device
(`/nix/var/nix/profiles/liar`) and streams your files to `~/.local/share/liar` of
the session user (`--user`, default `brick`). The game runs in that folder, so
saves (`liar2_N.sav`, `liar2.cfg`, `shopitem.cfg`) sit beside its data. `--remove`
takes the loader away and leaves files and saves.

Add the entry to your system with this flake's module, next to Mortar's core:

    mortar.lib.mkSystem { ports = [ liar.nixosModules.default ]; }

The entry appears under Games once the files are there. Other nixpkgs configurations
must allow the unfree package name `liar` (the module does it through `mortar.unfree`).

`flake.nix` points at `github:gopherbone/mortar`. To use a local checkout instead, add
`--override-input mortar git+file:///path/to/mortar` to each command.

## Controls

By the Brick's labels (the Wiz had the same names):

    D-pad    move
    B        OK, talk, attack       X        cancel, skip, minimap
    A        items                  Y        game menu (save, load, magic)
    L1, L2   the game's L           R1, R2   the game's R (change character)
    START    the Wiz's MENU (quit)  SELECT   the Wiz's SELECT
    MENU     the shell's

The game's own prompts and Help page name the same letters. (On the Wiz, B was the
right-hand button; on the Brick it's the bottom one.)

Picture: 960x720 (exact 3x) scaled to the panel by the compositor; set
`LIAR_SCALE=integer` in the entry's env for 3x with black borders instead.

## What has been checked

It has run on one Brick with one copy of the English build, and the installer says
when your executable isn't that build (it may still work). **Not verified:** combat,
magic, PNG alpha effects, sound by ear, suspend. The standalone installer here was
split out of Mortar's CLI and has been compiled and unit-tested, not yet run against
a device.
