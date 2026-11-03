# SPDX-License-Identifier: GPL-2.0-or-later
# Redemption: Liar, the retail GP2X Wiz RPG (English build, Liar_e), on the
# Brick, from the owner's own files.
#
# This package contains NO game data and no Wiz firmware. It is a loader and
# replacements for what the game links against: `liar-run` maps the owner's
# liar_e.gpe (a 32-bit old-ABI ARM executable) and runs it natively in AArch32,
# with its imports resolved to src/: libc wrappers (main.c), the SDL 1.2 it uses
# (sdl.c, image.c), a Wayland client and the pad (display.c), sound (audio.c) and
# a stand-in for the INKA DRM libraries (drm.c). See those files' headers.
#
# `liar-run` is a static armv7 binary with no 32-bit libraries beside it: the
# Brick's graphics and sound libraries are 64-bit, so it talks to the compositor
# over the Wayland socket itself and pipes sound into the system's 64-bit pw-cat.
#
# It is not part of the system closure (CI never builds it, the update cache never
# holds it); `nix run .#install -- --data PATH --host HOST` builds it, installs it
# into its own profile, /nix/var/nix/profiles/liar, and copies the owner's files
# to ~/.local/share/liar of the session user (module.nix). The game runs in that
# folder, so saves (liar2_N.sav, liar2.cfg, shopitem.cfg) sit beside its data.
#
# Controls, by the Brick's labels (the Wiz had the same names):
#   D-pad    move
#   B        OK, talk, attack       X        cancel, skip, minimap
#   A        items                  Y        game menu (save, load, magic)
#   L1, L2   the game's L           R1, R2   the game's R (change character)
#   START    the Wiz's MENU (quit)  SELECT   the Wiz's SELECT
#   MENU     the shell's
# The game's own prompts and Help page name the same letters. (On the Wiz, B was
# the right-hand button; on the Brick it's the bottom one.)
#
# Picture: 960x720 (exact 3x) scaled to the panel by the compositor; set
# LIAR_SCALE=integer in the entry's env for 3x with black borders instead.
{ lib, stdenvNoCC, buildPackages, pkgsCross, stb, pipewire, nukeReferences }:

let
  guestPkgs = pkgsCross.armv7l-hf-multiplatform;
  guest = guestPkgs.stdenv.mkDerivation {
    pname = "liar-run";
    version = "1";
    src = ./src;
    buildInputs = [ guestPkgs.glibc.static ];
    dontConfigure = true;
    buildPhase = ''
      runHook preBuild
      # At 0x40000000, out of the way of the game (it wants 0x8000-0x381640).
      $CC -std=gnu11 -O2 -g0 -Wall -Wextra -Wno-unused-parameter -marm \
        -mtune=cortex-a53 -mfpu=neon-vfpv4 \
        -I${stb}/include/stb -I. \
        -fno-pie -static -no-pie -Wl,-Ttext-segment=0x40000000 \
        -pthread -o liar-run main.c sdl.c image.c display.c audio.c drm.c thunk.S
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      install -Dm0755 liar-run $out/bin/liar-run
      runHook postInstall
    '';
    # static, and for another architecture than the tools around it
    dontPatchELF = true;
    hardeningDisable = [ "all" ];
  };
in
stdenvNoCC.mkDerivation {
  pname = "liar";
  version = "1";
  dontUnpack = true;
  nativeBuildInputs = [ nukeReferences ];
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/libexec/liar
    # Static: the paths to its toolchain's glibc that are left in it are never
    # used, and would otherwise pull 32-bit glibc into the closure.
    cp ${guest}/bin/liar-run $out/libexec/liar/liar-run
    chmod u+w $out/libexec/liar/liar-run
    nuke-refs $out/libexec/liar/liar-run
    cat > $out/bin/liar <<EOF2
    #!${buildPackages.runtimeShell}
    # Redemption: Liar: runs in \$XDG_DATA_HOME/liar, where the owner's files
    # and the saves are.
    set -e
    data="\''${XDG_DATA_HOME:-\$HOME/.local/share}/liar"
    if [ ! -e "\$data/liar_e.gpe" ]; then
      echo "liar: no game files in \$data: copy them from your card with the installer (see the README)" >&2
      exit 1
    fi
    cd "\$data"
    export LIAR_PW_CAT=${pipewire}/bin/pw-cat
    exec $out/libexec/liar/liar-run liar_e.gpe
    EOF2
    chmod +x $out/bin/liar
    runHook postInstall
  '';
  dontStrip = true;
  dontPatchELF = true;
  meta = {
    description = "Runs the owner's retail GP2X Wiz copy of Redemption: Liar on the Brick; contains no game data";
    # The loader is GPL, but it only exists to run a commercial game and its DRM
    # replacement hasn't been cleared for publishing: keep it off CI and caches
    # (module.nix adds it to `mortar.unfree`).
    license = lib.licenses.unfree;
    platforms = [ "aarch64-linux" ];
    mainProgram = "liar";
  };
}
