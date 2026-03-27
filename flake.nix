{ description = "ant dev shell";

inputs = {
  nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  flake-utils.url = "github:numtide/flake-utils";
};

outputs = { self, nixpkgs, flake-utils }: flake-utils.lib.eachDefaultSystem (system:
let pkgs = nixpkgs.legacyPackages.${system}; in {
  devShells.default = pkgs.mkShell {
  packages = with pkgs; [ pkg-config libossp_uuid libsodium openssl ];
  
  shellHook = ''
    export CC="ccache clang"
    export MACOSX_DEPLOYMENT_TARGET="15.0"
  '';
  };
}); }
