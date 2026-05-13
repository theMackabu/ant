{
  description = "Ant JavaScript runtime";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        ant = pkgs.callPackage ./nix/package.nix { };
      in {
        packages.default = ant;
        packages.ant = ant;
        
        devShells.default = pkgs.mkShell {
          inputsFrom = [ ant ];
          packages = with pkgs; [ ccache lld ];
        };
      });
}
