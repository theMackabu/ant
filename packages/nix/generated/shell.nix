# Generated from packages/nix/src/shell.ts; run ant packages/nix/generate.ts.
{ pkgs, toolchain }:
let
  optArgs = pkgs.lib.concatStringsSep " " [
    (if pkgs.stdenv.hostPlatform.isx86 then "-march=native" else "-mcpu=native")
    "-Qunused-arguments"
    "-fvisibility=hidden"
    "-fvisibility-inlines-hidden"
    "-fno-math-errno"
    "-fno-trapping-math"
    "-fno-stack-protector"
    "-mllvm"
    "-enable-machine-outliner=never"
  ];
in
pkgs.mkShellNoCC ({
  packages = [
    pkgs.nodejs_22
    toolchain.bintools
    toolchain.clang
    toolchain.compilerRt
    toolchain.llvm
  ];
  CFLAGS = optArgs;
  CXXFLAGS = optArgs;
  NIX_CFLAGS_COMPILE = optArgs;
  NIX_ENFORCE_NO_NATIVE = "0";
  LDFLAGS = "-resource-dir=${toolchain.compilerRt}";
  CC = "${toolchain.clang}/bin/clang";
  CXX = "${toolchain.clang}/bin/clang++";
} // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
  LD = "${toolchain.bintools}/bin/ld";
  AR = "${toolchain.bintools}/bin/ar";
  RANLIB = "${toolchain.bintools}/bin/ranlib";
  STRIP = "${toolchain.bintools}/bin/strip";
  shellHook = ''
    export SDKROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk"
  '';
})
