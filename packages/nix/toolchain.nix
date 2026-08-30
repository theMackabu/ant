{ pkgs }: {
  clang = pkgs.llvmPackages_21.clang-unwrapped.out;
  compilerRt = pkgs.llvmPackages_21.compiler-rt;
  llvm = pkgs.llvmPackages_21.llvm;
  bintools =
    if pkgs.stdenv.hostPlatform.isDarwin
    then pkgs.darwin.binutils-unwrapped
    else pkgs.llvmPackages_21.bintools;
  inherit (pkgs.llvmPackages_21) stdenv;
}
