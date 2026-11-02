{ pkgs, toolchain, nodejs }:

let
  lib = pkgs.lib;
  isDarwin = pkgs.stdenv.hostPlatform.isDarwin;
  darwinClang = pkgs.llvmPackages_21.clang;
  nativeTuneFlag =
    if pkgs.stdenv.hostPlatform.isx86
    then "-march=native"
    else "-mcpu=native";
  optFlags = [
    nativeTuneFlag
    "-Qunused-arguments"
    "-fvisibility=hidden"
    "-fvisibility-inlines-hidden"
    "-fno-math-errno"
    "-fno-trapping-math"
    "-fno-stack-protector"
    "-mllvm"
    "-enable-machine-outliner=never"
  ];
  optArgs = lib.concatStringsSep " " optFlags;
in
pkgs.mkShellNoCC ({
  packages = [ nodejs ] ++ (if isDarwin then [ darwinClang ] else [
    toolchain.bintools
    toolchain.clang
    toolchain.compilerRt
  ]) ++ [ toolchain.llvm ];

  CFLAGS = optArgs;
  CXXFLAGS = optArgs;
  NIX_ENFORCE_NO_NATIVE = "0";
  hardeningDisable = [ "all" ];
} // (if isDarwin then {
  CC = "${darwinClang}/bin/clang";
  CXX = "${darwinClang}/bin/clang++";
} else {
  NIX_CFLAGS_COMPILE = optArgs;
  LDFLAGS = "-resource-dir=${toolchain.compilerRt}";

  CC = "${toolchain.clang}/bin/clang";
  CXX = "${toolchain.clang}/bin/clang++";
}))
