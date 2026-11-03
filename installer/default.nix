# SPDX-License-Identifier: GPL-2.0-or-later
# The installer behind `nix run .#install`. `nix` is taken from the user's PATH on
# purpose (their own install and builder configuration); ssh comes with it.
{ lib, rustPlatform, makeWrapper, openssh, loader }:

rustPlatform.buildRustPackage {
  pname = "liar-install";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [ ./Cargo.toml ./Cargo.lock ./src ];
  };
  cargoLock.lockFile = ./Cargo.lock;
  nativeBuildInputs = [ makeWrapper ];
  postInstall = ''
    wrapProgram $out/bin/liar-install \
      --suffix PATH : ${lib.makeBinPath [ openssh ]} \
      --set LIAR_LOADER ${loader}
  '';
  meta = {
    license = lib.licenses.gpl2Plus;
    mainProgram = "liar-install";
  };
}
