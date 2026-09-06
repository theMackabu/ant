# Generated from packages/nix/src/toolchain.ts; run ant packages/nix/generate.ts.
{ pkgs }: {
  clang = pkgs.llvmPackages_21.clang-unwrapped.out;
  compilerRt = pkgs.llvmPackages_21.compiler-rt;
  llvm = pkgs.llvmPackages_21.llvm;
  bintools =
    if pkgs.stdenv.hostPlatform.isDarwin
    then pkgs.darwin.binutils-unwrapped
    else pkgs.llvmPackages_21.bintools;
  stdenv = pkgs.llvmPackages_21.stdenv;
}
