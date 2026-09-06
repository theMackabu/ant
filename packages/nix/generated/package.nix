# Generated from packages/nix/src/package.ts; run ant packages/nix/generate.ts.
{
  lib,
  llvmPackages_21,
  stdenv,
  ccache,
  meson,
  ninja,
  cmake,
  pkg-config,
  python3,
  nodejs_22,
  git,
  curl,
  zig_0_16 ? null,
  zig ? null,
  importNpmLock,
  overrideCC,
  rustPlatform,
  rustToolchain,
  runCommand,
  writeText,
  darwin ? null,
  callPackage,
  gitRev ? "unknown",
  enablePgo ? false,
  enableNativeTuning ? false,
}:
let
  antBaseStdenv =
    if stdenv.hostPlatform.isLinux
    then overrideCC llvmPackages_21.stdenv (llvmPackages_21.stdenv.cc.override {
      bintools = llvmPackages_21.bintools;
    })
    else stdenv;

  ccacheLinks = (ccache.links {
    unwrappedCC = antBaseStdenv.cc.cc;
    extraConfig = ''
      export CCACHE_COMPRESS=1
      export CCACHE_MAXSIZE=2G
      export CCACHE_SLOPPINESS=random_seed,time_macros
      if [ -d /tmp/ant-nix-ccache ] && [ -w /tmp/ant-nix-ccache ]; then
        export CCACHE_DIR=/tmp/ant-nix-ccache
      else
        export CCACHE_DIR="$TMPDIR/ccache"
      fi
    '';
  }).overrideAttrs (previous: {
    passthru = (previous.passthru or {}) // {
      langC = antBaseStdenv.cc.cc.langC or true;
      langCC = antBaseStdenv.cc.cc.langCC or true;
    };
  });

  antStdenv = overrideCC antBaseStdenv (antBaseStdenv.cc.override {
    cc = ccacheLinks;
  });

  temporalCargoDeps = rustPlatform.fetchCargoVendor {
    src = ../../..;
    cargoRoot = "src/temporal";
    name = "ant-temporal-cargo-deps";
    hash = "sha256-7Ny7Y3VdOB5GFS2SoUUhqIzTJsVRdjsPTon9ndKm5RA=";
  };

  rustStdCargoDeps = rustPlatform.fetchCargoVendor {
    src = rustToolchain;
    cargoRoot = "lib/rustlib/src/rust/library";
    name = "ant-rust-std-cargo-deps";
    hash = "sha256-kUUC6D6xrFap7+gn+lq1i3lBawfzbnUmDfB5QIvLnYA=";
  };

  temporalBuildCargoDeps = runCommand "ant-temporal-build-cargo-deps" {} ''
    mkdir -p "$out"
    cp -R ${rustStdCargoDeps}/. "$out/"
    chmod -R u+w "$out"
    cp -R ${temporalCargoDeps}/. "$out/"
  '';

  pgoFileName =
    "ant-${stdenv.hostPlatform.parsed.kernel.name}-${stdenv.hostPlatform.parsed.cpu.name}.profdata";

  antVendor = callPackage ./vendor.nix {};

  mesonNativeFile = writeText "ant-meson-native.ini" ''
    [binaries]
    c = '${antStdenv.cc}/bin/clang'
    cpp = '${antStdenv.cc}/bin/clang++'
  '';

  toolsNodeModules = importNpmLock.buildNodeModules {
    package = lib.importJSON ../../../src/tools/package.json;
    packageLock = lib.importJSON ../../../src/tools/npm-shrinkwrap.json;
    nodejs = nodejs_22;
  };
in
antStdenv.mkDerivation (finalAttrs:
{
  pname = "ant";
  src = ../../..;
  version = import ./version.nix {
    lib = lib;
    gitRev = gitRev;
  };
  nativeBuildInputs = [
    meson
    ninja
    cmake
    pkg-config
    python3
    nodejs_22
    git
    curl
    (if zig_0_16 != null then zig_0_16 else zig)
    rustPlatform.cargoSetupHook
  ] ++ (lib.optionals stdenv.hostPlatform.isDarwin [ darwin.sigtool llvmPackages_21.llvm ]);
  cargoDeps = temporalBuildCargoDeps;
  cargoRoot = "src/temporal";
  postUnpack = ''
    chmod -R u+w "$sourceRoot/vendor"
    cp -rT --no-preserve=mode ${antVendor} "$sourceRoot/vendor"
    chmod -R u+w "$sourceRoot/vendor"
  '';
  mesonFlags = [
    "--native-file=${mesonNativeFile}"
    "-Dbuild_git_hash=${gitRev}"
    "-Db_lto_mode=default"
    "-Dembed_example=disabled"
  ] ++ (lib.optionals
    stdenv.hostPlatform.isDarwin
    [ "-Dllvm_nm=${lib.getExe' llvmPackages_21.llvm "llvm-nm"}" ]) ++ (lib.optionals enableNativeTuning [ "-Dnative_tuning=enabled" ]) ++ (if enablePgo
  then if builtins.pathExists (../../../meson/pgo/profiles + "/${pgoFileName}")
  then [ "-Dpgo=enabled" ]
  else throw "enablePgo requested but missing PGO profile: meson/pgo/profiles/${pgoFileName}"
  else [ "-Dpgo=disabled" ]);
  env = {
    ANT_TEMPORAL_CARGO = lib.getExe' rustToolchain "cargo";
    RUSTC = lib.getExe' rustToolchain "rustc";
    HOST_CC = "${antStdenv.cc}/bin/clang";
    HOST_CXX = "${antStdenv.cc}/bin/clang++";
    NIX_CFLAGS_COMPILE = lib.concatStringsSep " " [
      "-Qunused-arguments"
      "-fvisibility=hidden"
      "-fvisibility-inlines-hidden"
      "-fno-math-errno"
      "-fno-trapping-math"
      "-fno-stack-protector"
      "-mllvm"
      "-enable-machine-outliner=never"
    ];
  } // {
    "CARGO_TARGET_${stdenv.hostPlatform.rust.cargoEnvVarTarget}_LINKER" =
      "${antStdenv.cc}/bin/clang";
  } // {
    "CC_${stdenv.hostPlatform.rust.cargoEnvVarTarget}" = "${antStdenv.cc}/bin/clang";
  } // {
    "CXX_${stdenv.hostPlatform.rust.cargoEnvVarTarget}" = "${antStdenv.cc}/bin/clang++";
  };
  preConfigure = ''
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
    export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

    ln -sfn ${toolsNodeModules}/node_modules src/tools/node_modules
  '' + (lib.optionalString enablePgo ''
    echo "==> PGO profile available: meson/pgo/profiles/${pgoFileName}"
  '');
  installPhase = ''
    runHook preInstall
    install -Dm755 ant "$out/bin/ant"
    ln -s ant "$out/bin/antx"
    runHook postInstall
  '';
  postFixup = lib.optionalString stdenv.hostPlatform.isDarwin ''
    strip -S -x "$out/bin/ant"
    codesign --force --sign - --entitlements ${../../../meson/ant.entitlements} "$out/bin/ant"
  '';
  doCheck = false;
  meta = {
    description = "Ant JavaScript runtime";
    homepage = "https://github.com/themackabu/ant";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
    mainProgram = "ant";
  };
} // lib.optionalAttrs enableNativeTuning {
  NIX_ENFORCE_NO_NATIVE = false;
})
