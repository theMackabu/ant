{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  packages = with pkgs; [ pkg-config libsodium libossp_uuid openssl.dev];
  
  shellHook = ''
    export CC="ccache clang"
    export MACOSX_DEPLOYMENT_TARGET="15.0"
  '';
}