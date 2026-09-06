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
  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
  (import ./packages/nix/generated/flake.nix).outputs {
    self = self;
    nixpkgs = nixpkgs;
    flake-utils = flake-utils;
    rust-overlay = rust-overlay;
  };
}
