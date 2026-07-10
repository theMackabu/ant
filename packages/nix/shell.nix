{ pkgs, toolchain }:

let
  lib = pkgs.lib;
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
  packages = [
    toolchain.bintools
    toolchain.clang
    toolchain.compilerRt
  ];

  CFLAGS = optArgs;
  CXXFLAGS = optArgs;
  NIX_CFLAGS_COMPILE = optArgs;
  NIX_ENFORCE_NO_NATIVE = "0";
  LDFLAGS = "-resource-dir=${toolchain.compilerRt}";

  CC = "${toolchain.clang}/bin/clang";
  CXX = "${toolchain.clang}/bin/clang++";
} // lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
  LD = "${toolchain.bintools}/bin/ld";
  AR = "${toolchain.bintools}/bin/ar";
  RANLIB = "${toolchain.bintools}/bin/ranlib";
  STRIP = "${toolchain.bintools}/bin/strip";

  shellHook = ''
    export SDKROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk"
  '';
})
