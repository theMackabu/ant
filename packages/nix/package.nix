{ lib
, llvmPackages_21
, stdenv
, ccacheStdenv
, meson
, ninja
, cmake
, pkg-config
, python3
, nodejs_22
, git
, curl
, zig_0_16 ? null
, zig ? null
, importNpmLock
, overrideCC
, rustPlatform
, rustToolchain
, runCommand
, writeText
, darwin ? null
, callPackage
, gitRev ? "unknown"
, enablePgo ? false
, enableNativeTuning ? false
}:

let
  zigPkg = if zig_0_16 != null then zig_0_16 else zig;
  antBaseStdenv =
    if stdenv.isLinux then
      overrideCC llvmPackages_21.stdenv (
        llvmPackages_21.stdenv.cc.override { bintools = llvmPackages_21.bintools; }
      )
    else stdenv;
  antStdenv = ccacheStdenv.override {
    stdenv = antBaseStdenv;
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
  };

  antVersion = import ./version.nix { inherit lib gitRev; };
  antVendor = callPackage ./vendor.nix {};
  cargoRoot = "src/temporal";
  temporalCargoDeps = rustPlatform.fetchCargoVendor {
    src = ../..;
    inherit cargoRoot;
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

  mesonNativeFile = writeText "ant-meson-native.ini" ''
    [binaries]
    c = '${antStdenv.cc}/bin/clang'
    cpp = '${antStdenv.cc}/bin/clang++'
  '';

  toolsNodeModules = importNpmLock.buildNodeModules {
    package = lib.importJSON ../../src/tools/package.json;
    packageLock = lib.importJSON ../../src/tools/npm-shrinkwrap.json;
    nodejs = nodejs_22;
  };

  extraOptFlags = [
    "-Qunused-arguments"
    "-fvisibility=hidden"
    "-fvisibility-inlines-hidden"
    "-fno-math-errno"
    "-fno-trapping-math"
    "-fno-stack-protector"
    "-mllvm" "-enable-machine-outliner=never"
  ];
  optArgs = lib.concatStringsSep " " extraOptFlags;

  pgoFileName = "ant-${stdenv.hostPlatform.parsed.kernel.name}-${stdenv.hostPlatform.parsed.cpu.name}.profdata";
  pgoProfile = ../../meson/pgo/profiles + "/${pgoFileName}";
  pgoProfileExists = builtins.pathExists pgoProfile;
  pgoFlags =
    if enablePgo then
      if pgoProfileExists then [ "-Dpgo=enabled" ]
      else throw "enablePgo requested but missing PGO profile: meson/pgo/profiles/${pgoFileName}"
    else [ "-Dpgo=disabled" ];
in

antStdenv.mkDerivation (finalAttrs: {
  pname = "ant";
  src = ../..;
  version = antVersion;

  nativeBuildInputs = [
    meson
    ninja
    cmake
    pkg-config
    python3
    nodejs_22
    git
    curl
    zigPkg
    rustPlatform.cargoSetupHook
  ] ++ lib.optionals stdenv.isDarwin [
    darwin.sigtool
    llvmPackages_21.llvm
  ];

  cargoDeps = temporalBuildCargoDeps;

  inherit cargoRoot;

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
  ] ++ lib.optionals stdenv.isDarwin [
    "-Dllvm_nm=${lib.getExe' llvmPackages_21.llvm "llvm-nm"}"
  ] ++ lib.optionals enableNativeTuning [
    "-Dnative_tuning=enabled"
  ] ++ pgoFlags;

  env = {
    ANT_TEMPORAL_CARGO = lib.getExe' rustToolchain "cargo";
    RUSTC = lib.getExe' rustToolchain "rustc";
    NIX_CFLAGS_COMPILE = optArgs;
  };

  preConfigure = ''
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
    export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

    ln -sfn ${toolsNodeModules}/node_modules src/tools/node_modules
  '' + lib.optionalString enablePgo ''
    echo "==> PGO profile available: meson/pgo/profiles/${pgoFileName}"
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 ant "$out/bin/ant"
    ln -s ant "$out/bin/antx"
    runHook postInstall
  '';

  postFixup = lib.optionalString stdenv.isDarwin ''
    strip -S -x "$out/bin/ant"
    codesign --force --sign - --entitlements ${../../meson/ant.entitlements} "$out/bin/ant"
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
