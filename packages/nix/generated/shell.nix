# Generated from packages/nix/src/shell.ts; run ant packages/nix/generate.ts.
({ pkgs, toolchain }: (let
  __tsnix_0_pkgs = pkgs;
  __tsnix_1_toolchain = toolchain;
in (let
  __tsnix_2_optArgs = ((((__tsnix_0_pkgs).lib).concatStringsSep) (" ") ([ ((if (((__tsnix_0_pkgs).stdenv.hostPlatform).isx86) then ("-march=native") else ("-mcpu=native"))) "-Qunused-arguments" "-fvisibility=hidden" "-fvisibility-inlines-hidden" "-fno-math-errno" "-fno-trapping-math" "-fno-stack-protector" "-mllvm" "-enable-machine-outliner=never" ]));
in (((__tsnix_0_pkgs).mkShellNoCC) ((({
  packages = [ ((__tsnix_0_pkgs).nodejs_22) ((__tsnix_1_toolchain).bintools) ((__tsnix_1_toolchain).clang) ((__tsnix_1_toolchain).compilerRt) ((__tsnix_1_toolchain).llvm) ];
  CFLAGS = __tsnix_2_optArgs;
  CXXFLAGS = __tsnix_2_optArgs;
  NIX_CFLAGS_COMPILE = __tsnix_2_optArgs;
  NIX_ENFORCE_NO_NATIVE = "0";
  LDFLAGS = "-resource-dir=${(__tsnix_1_toolchain).compilerRt}";
  CC = "${(__tsnix_1_toolchain).clang}/bin/clang";
  CXX = "${(__tsnix_1_toolchain).clang}/bin/clang++";
}) // (((((__tsnix_0_pkgs).lib).optionalAttrs) (((__tsnix_0_pkgs).stdenv.hostPlatform).isDarwin) ({
  LD = "${(__tsnix_1_toolchain).bintools}/bin/ld";
  AR = "${(__tsnix_1_toolchain).bintools}/bin/ar";
  RANLIB = "${(__tsnix_1_toolchain).bintools}/bin/ranlib";
  STRIP = "${(__tsnix_1_toolchain).bintools}/bin/strip";
  shellHook = "export SDKROOT=\"/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk\"\n";
})))))))))
