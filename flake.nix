{
  description = "javascript for 🐜's, a tiny runtime with big ambitions";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        ant = pkgs.callPackage ./nix/package.nix {
          gitRev = self.shortRev or self.dirtyShortRev or "unknown";
        };
      in {
        packages.default = ant;
        packages.ant = ant;
        
        devShells.default = pkgs.mkShell {
          inputsFrom = [ ant ];
          packages = with pkgs; [ ccache lld ];
        };
      });
}
