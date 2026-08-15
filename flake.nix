{
  description = "javascript for 🐜's, a tiny runtime with big ambitions";

  nixConfig = {
    extra-substituters = ["https://ant.cachix.org"];
    extra-trusted-public-keys = ["ant.cachix.org-1:v/FbrMBfZ/rZHKZtAqM5mpLu6YKLaDF64dcLP30VTH0="];
  };

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
  flake-utils.lib.eachDefaultSystem (system:
    let
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ rust-overlay.overlays.default ];
      };
      toolchain = import ./packages/nix/toolchain.nix { inherit pkgs; };
      rustToolchain = pkgs.rust-bin.fromRustupToolchainFile ./src/temporal/rust-toolchain.toml;
      rustPlatform = pkgs.makeRustPlatform {
        cargo = rustToolchain;
        rustc = rustToolchain;
      };
      ant = pkgs.callPackage ./packages/nix/package.nix {
        gitRev = self.shortRev or self.dirtyShortRev or "unknown";
        stdenv = toolchain.stdenv;
        inherit rustPlatform rustToolchain;
      };
    in {
      packages.default = ant;
      packages.ant = ant;
      
      devShells.default = import ./packages/nix/shell.nix {
        inherit pkgs toolchain;
      };
    });
}
