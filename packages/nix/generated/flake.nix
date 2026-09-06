# Generated from packages/nix/src/flake.ts; run ant packages/nix/generate.ts.
{
  description = "javascript for 🐜's, a tiny runtime with big ambitions";
  nixConfig = {
    extra-substituters = [ "https://ant.cachix.org" ];
    extra-trusted-public-keys = [ "ant.cachix.org-1:v/FbrMBfZ/rZHKZtAqM5mpLu6YKLaDF64dcLP30VTH0=" ];
  };
  inputs = {
    nixpkgs = {
      url = "github:NixOS/nixpkgs/nixos-unstable";
    };
    flake-utils = {
      url = "github:numtide/flake-utils";
    };
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs = {
        nixpkgs = {
          follows = "nixpkgs";
        };
      };
    };
  };
  outputs = ({ self, nixpkgs, flake-utils, rust-overlay }: (let
  __tsnix_0_self = self;
  __tsnix_1_nixpkgs = nixpkgs;
  __tsnix_2_flake-utils = flake-utils;
  __tsnix_3_rust-overlay = rust-overlay;
in (((__tsnix_2_flake-utils).lib.eachDefaultSystem) ((__tsnix_4_system: (let
  __tsnix_5_pkgs = ((import) (__tsnix_1_nixpkgs) ({
  system = __tsnix_4_system;
  overlays = [ ((__tsnix_3_rust-overlay).overlays.default) ];
}));
in (let
  __tsnix_6_toolchain = ((import) (./toolchain.nix) ({
  pkgs = __tsnix_5_pkgs;
}));
  __tsnix_7_rustToolchain = (((__tsnix_5_pkgs).rust-bin.fromRustupToolchainFile) (../../../src/temporal/rust-toolchain.toml));
  __tsnix_8_rustPlatform = (((__tsnix_5_pkgs).makeRustPlatform) ({
  cargo = __tsnix_7_rustToolchain;
  rustc = __tsnix_7_rustToolchain;
}));
  __tsnix_9_ant = (((__tsnix_5_pkgs).callPackage) (./package.nix) ({
  gitRev = ((__tsnix_0_self).shortRev or (((__tsnix_0_self).dirtyShortRev or ("unknown"))));
  stdenv = (__tsnix_6_toolchain).stdenv;
  rustPlatform = __tsnix_8_rustPlatform;
  rustToolchain = __tsnix_7_rustToolchain;
}));
in {
  packages = {
    ant = __tsnix_9_ant;
    default = __tsnix_9_ant;
  };
  devShells = {
    default = ((import) (./shell.nix) ({
  pkgs = __tsnix_5_pkgs;
  toolchain = __tsnix_6_toolchain;
}));
  };
})))))));
}
