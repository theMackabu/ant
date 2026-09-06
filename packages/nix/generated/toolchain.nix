# Generated from packages/nix/src/toolchain.ts; run ant packages/nix/generate.ts.
({ pkgs }: (let
  __tsnix_0_pkgs = pkgs;
in {
  clang = ((__tsnix_0_pkgs).llvmPackages_21).clang-unwrapped.out;
  compilerRt = ((__tsnix_0_pkgs).llvmPackages_21).compiler-rt;
  llvm = ((__tsnix_0_pkgs).llvmPackages_21).llvm;
  bintools = (if ((__tsnix_0_pkgs).stdenv.hostPlatform.isDarwin) then ((__tsnix_0_pkgs).darwin.binutils-unwrapped) else (((__tsnix_0_pkgs).llvmPackages_21).bintools));
  stdenv = ((__tsnix_0_pkgs).llvmPackages_21).stdenv;
}))
