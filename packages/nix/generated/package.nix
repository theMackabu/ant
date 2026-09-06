# Generated from packages/nix/src/package.ts; run ant packages/nix/generate.ts.
({ lib, llvmPackages_21, stdenv, ccache, meson, ninja, cmake, pkg-config, python3, nodejs_22, git, curl, zig_0_16 ? (null), zig ? (null), importNpmLock, overrideCC, rustPlatform, rustToolchain, runCommand, writeText, darwin ? (null), callPackage, gitRev ? ("unknown"), enablePgo ? (false), enableNativeTuning ? (false) }: (let
  __tsnix_0_lib = lib;
  __tsnix_1_llvmPackages_21 = llvmPackages_21;
  __tsnix_2_stdenv = stdenv;
  __tsnix_3_ccache = ccache;
  __tsnix_4_meson = meson;
  __tsnix_5_ninja = ninja;
  __tsnix_6_cmake = cmake;
  __tsnix_7_pkg-config = pkg-config;
  __tsnix_8_python3 = python3;
  __tsnix_9_nodejs_22 = nodejs_22;
  __tsnix_10_git = git;
  __tsnix_11_curl = curl;
  __tsnix_12_zig_0_16 = zig_0_16;
  __tsnix_13_zig = zig;
  __tsnix_14_importNpmLock = importNpmLock;
  __tsnix_15_overrideCC = overrideCC;
  __tsnix_16_rustPlatform = rustPlatform;
  __tsnix_17_rustToolchain = rustToolchain;
  __tsnix_18_runCommand = runCommand;
  __tsnix_19_writeText = writeText;
  __tsnix_20_darwin = darwin;
  __tsnix_21_callPackage = callPackage;
  __tsnix_22_gitRev = gitRev;
  __tsnix_23_enablePgo = enablePgo;
  __tsnix_24_enableNativeTuning = enableNativeTuning;
in (let
  __tsnix_25_antBaseStdenv = (if ((__tsnix_2_stdenv).hostPlatform.isLinux) then (((__tsnix_15_overrideCC) ((__tsnix_1_llvmPackages_21).stdenv) ((((__tsnix_1_llvmPackages_21).stdenv.cc.override) ({
  bintools = (__tsnix_1_llvmPackages_21).bintools;
}))))) else (__tsnix_2_stdenv));
  __tsnix_27_ccacheLinks = ((((((__tsnix_3_ccache).links) ({
  unwrappedCC = (__tsnix_25_antBaseStdenv).cc.cc;
  extraConfig = "export CCACHE_COMPRESS=1\nexport CCACHE_MAXSIZE=2G\nexport CCACHE_SLOPPINESS=random_seed,time_macros\nif [ -d /tmp/ant-nix-ccache ] && [ -w /tmp/ant-nix-ccache ]; then\n  export CCACHE_DIR=/tmp/ant-nix-ccache\nelse\n  export CCACHE_DIR=\"$TMPDIR/ccache\"\nfi\n";
}))).overrideAttrs) ((__tsnix_26_previous: {
  passthru = ((((__tsnix_26_previous).passthru or ({}))) // ({
  langC = (((__tsnix_25_antBaseStdenv).cc.cc).langC or (true));
  langCC = (((__tsnix_25_antBaseStdenv).cc.cc).langCC or (true));
}));
})));
  __tsnix_28_antStdenv = ((__tsnix_15_overrideCC) (__tsnix_25_antBaseStdenv) ((((__tsnix_25_antBaseStdenv).cc.override) ({
  cc = __tsnix_27_ccacheLinks;
}))));
  __tsnix_29_temporalCargoDeps = (((__tsnix_16_rustPlatform).fetchCargoVendor) ({
  src = ../../..;
  cargoRoot = "src/temporal";
  name = "ant-temporal-cargo-deps";
  hash = "sha256-7Ny7Y3VdOB5GFS2SoUUhqIzTJsVRdjsPTon9ndKm5RA=";
}));
  __tsnix_30_rustStdCargoDeps = (((__tsnix_16_rustPlatform).fetchCargoVendor) ({
  src = __tsnix_17_rustToolchain;
  cargoRoot = "lib/rustlib/src/rust/library";
  name = "ant-rust-std-cargo-deps";
  hash = "sha256-kUUC6D6xrFap7+gn+lq1i3lBawfzbnUmDfB5QIvLnYA=";
}));
  __tsnix_31_temporalBuildCargoDeps = ((__tsnix_18_runCommand) ("ant-temporal-build-cargo-deps") ({}) ("mkdir -p \"$out\"\ncp -R ${__tsnix_30_rustStdCargoDeps}/. \"$out/\"\nchmod -R u+w \"$out\"\ncp -R ${__tsnix_29_temporalCargoDeps}/. \"$out/\"\n"));
  __tsnix_32_pgoFileName = "ant-${((__tsnix_2_stdenv).hostPlatform.parsed).kernel.name}-${((__tsnix_2_stdenv).hostPlatform.parsed).cpu.name}.profdata";
  __tsnix_33_antVendor = ((__tsnix_21_callPackage) (./vendor.nix) ({}));
  __tsnix_34_mesonNativeFile = ((__tsnix_19_writeText) ("ant-meson-native.ini") ("[binaries]\nc = '${(__tsnix_28_antStdenv).cc}/bin/clang'\ncpp = '${(__tsnix_28_antStdenv).cc}/bin/clang++'\n"));
  __tsnix_35_toolsNodeModules = (((__tsnix_14_importNpmLock).buildNodeModules) ({
  package = (((__tsnix_0_lib).importJSON) (../../../src/tools/package.json));
  packageLock = (((__tsnix_0_lib).importJSON) (../../../src/tools/npm-shrinkwrap.json));
  nodejs = __tsnix_9_nodejs_22;
}));
in (((__tsnix_28_antStdenv).mkDerivation) ((__tsnix_36_finalAttrs: (({
  pname = "ant";
  src = ../../..;
  version = ((import) (./version.nix) ({
  lib = __tsnix_0_lib;
  gitRev = __tsnix_22_gitRev;
}));
  nativeBuildInputs = ([ (__tsnix_4_meson) (__tsnix_5_ninja) (__tsnix_6_cmake) (__tsnix_7_pkg-config) (__tsnix_8_python3) (__tsnix_9_nodejs_22) (__tsnix_10_git) (__tsnix_11_curl) ((if ((__tsnix_12_zig_0_16) != null) then (__tsnix_12_zig_0_16) else (__tsnix_13_zig))) ((__tsnix_16_rustPlatform).cargoSetupHook) ]) ++ ((((__tsnix_0_lib).optionals) ((__tsnix_2_stdenv).hostPlatform.isDarwin) ([ ((__tsnix_20_darwin).sigtool) ((__tsnix_1_llvmPackages_21).llvm) ])));
  cargoDeps = __tsnix_31_temporalBuildCargoDeps;
  cargoRoot = "src/temporal";
  postUnpack = "chmod -R u+w \"$sourceRoot/vendor\"\ncp -rT --no-preserve=mode ${__tsnix_33_antVendor} \"$sourceRoot/vendor\"\nchmod -R u+w \"$sourceRoot/vendor\"\n";
  mesonFlags = ([ ("--native-file=${__tsnix_34_mesonNativeFile}") ("-Dbuild_git_hash=${__tsnix_22_gitRev}") "-Db_lto_mode=default" "-Dembed_example=disabled" ]) ++ ((((__tsnix_0_lib).optionals) ((__tsnix_2_stdenv).hostPlatform.isDarwin) ([ ("-Dllvm_nm=${(((__tsnix_0_lib).getExe') ((__tsnix_1_llvmPackages_21).llvm) ("llvm-nm"))}") ]))) ++ ((((__tsnix_0_lib).optionals) (__tsnix_24_enableNativeTuning) ([ "-Dnative_tuning=enabled" ]))) ++ ((if (__tsnix_23_enablePgo) then ((if ((((builtins).pathExists) ((../../../meson/pgo/profiles) + ("/${__tsnix_32_pgoFileName}")))) then ([ "-Dpgo=enabled" ]) else (((throw) ("enablePgo requested but missing PGO profile: meson/pgo/profiles/${__tsnix_32_pgoFileName}"))))) else ([ "-Dpgo=disabled" ])));
  env = (((((({
  ANT_TEMPORAL_CARGO = (((__tsnix_0_lib).getExe') (__tsnix_17_rustToolchain) ("cargo"));
  RUSTC = (((__tsnix_0_lib).getExe') (__tsnix_17_rustToolchain) ("rustc"));
  HOST_CC = "${(__tsnix_28_antStdenv).cc}/bin/clang";
  HOST_CXX = "${(__tsnix_28_antStdenv).cc}/bin/clang++";
  NIX_CFLAGS_COMPILE = (((__tsnix_0_lib).concatStringsSep) (" ") ([ "-Qunused-arguments" "-fvisibility=hidden" "-fvisibility-inlines-hidden" "-fno-math-errno" "-fno-trapping-math" "-fno-stack-protector" "-mllvm" "-enable-machine-outliner=never" ]));
}) // ({ ${"CARGO_TARGET_${(__tsnix_2_stdenv).hostPlatform.rust.cargoEnvVarTarget}_LINKER"} = "${(__tsnix_28_antStdenv).cc}/bin/clang"; }))) // ({ ${"CC_${(__tsnix_2_stdenv).hostPlatform.rust.cargoEnvVarTarget}"} = "${(__tsnix_28_antStdenv).cc}/bin/clang"; }))) // ({ ${"CXX_${(__tsnix_2_stdenv).hostPlatform.rust.cargoEnvVarTarget}"} = "${(__tsnix_28_antStdenv).cc}/bin/clang++"; }));
  preConfigure = ("export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache\nexport ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache\nmkdir -p \"$ZIG_GLOBAL_CACHE_DIR\" \"$ZIG_LOCAL_CACHE_DIR\"\n\nln -sfn ${__tsnix_35_toolsNodeModules}/node_modules src/tools/node_modules\n") + ((((__tsnix_0_lib).optionalString) (__tsnix_23_enablePgo) ("echo \"==> PGO profile available: meson/pgo/profiles/${__tsnix_32_pgoFileName}\"\n")));
  installPhase = "runHook preInstall\ninstall -Dm755 ant \"$out/bin/ant\"\nln -s ant \"$out/bin/antx\"\nrunHook postInstall\n";
  postFixup = (((__tsnix_0_lib).optionalString) ((__tsnix_2_stdenv).hostPlatform.isDarwin) ("strip -S -x \"$out/bin/ant\"\ncodesign --force --sign - --entitlements ${../../../meson/ant.entitlements} \"$out/bin/ant\"\n"));
  doCheck = false;
  meta = {
    description = "Ant JavaScript runtime";
    homepage = "https://github.com/themackabu/ant";
    license = (__tsnix_0_lib).licenses.mit;
    platforms = (__tsnix_0_lib).platforms.unix;
    mainProgram = "ant";
  };
}) // ((((__tsnix_0_lib).optionalAttrs) (__tsnix_24_enableNativeTuning) ({
  NIX_ENFORCE_NO_NATIVE = false;
}))))))))))
