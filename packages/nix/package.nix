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
, apple-sdk_15 ? null
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
    else llvmPackages_21.stdenv;
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
  ] ++ lib.optionals stdenv.isDarwin [ darwin.sigtool ];

  buildInputs = lib.optionals stdenv.isDarwin [ apple-sdk_15 ];

  postUnpack = ''
    chmod -R u+w "$sourceRoot/vendor"
    cp -rT --no-preserve=mode ${antVendor} "$sourceRoot/vendor"
    chmod -R u+w "$sourceRoot/vendor"
  '';

  mesonFlags = [
    "-Dbuild_git_hash=${gitRev}"
    "-Db_lto_mode=default"
    "-Dembed_example=disabled"
  ] ++ lib.optionals enableNativeTuning [
    "-Dnative_tuning=enabled"
  ] ++ pgoFlags;

  env.NIX_CFLAGS_COMPILE = optArgs;

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
