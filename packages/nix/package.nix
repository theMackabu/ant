{ lib
, llvmPackages_21
, stdenv
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
, apple-sdk_15 ? null
, darwin ? null
, callPackage
, gitRev ? "unknown"
, enablePgo ? false
}:

let
  zigPkg = if zig_0_16 != null then zig_0_16 else zig;

  cpuTuneFlag =
    if stdenv.hostPlatform.isx86 then "-march=native" else "-mcpu=native";
  antVersion = import ./version.nix { inherit lib gitRev; };
  antVendor = callPackage ./vendor.nix { inherit gitRev; };

  toolsNodeModules = importNpmLock.buildNodeModules {
    package = lib.importJSON ../../src/tools/package.json;
    packageLock = lib.importJSON ../../src/tools/npm-shrinkwrap.json;
    nodejs = nodejs_22;
  };

  extraOptFlags = [
    cpuTuneFlag
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

llvmPackages_21.stdenv.mkDerivation (finalAttrs: {
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
  ] ++ lib.optionals stdenv.isLinux [
    llvmPackages_21.lld
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
  ] ++ lib.optionals stdenv.isLinux [
    "-Dc_link_args=-fuse-ld=lld"
    "-Dcpp_link_args=-fuse-ld=lld"
  ] ++ pgoFlags;

  NIX_ENFORCE_NO_NATIVE = false;
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
    install -Dm755 ant "$out/ant"
    ln -s ant "$out/antx"
    runHook postInstall
  '';

  postFixup = lib.optionalString stdenv.isDarwin ''
    strip -S -x "$out/ant"
    codesign --force --sign - --entitlements ${../../meson/ant.entitlements} "$out/ant"
  '';

  doCheck = false;

  meta = {
    description = "Ant JavaScript runtime";
    homepage = "https://github.com/themackabu/ant";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
    mainProgram = "ant";
  };
})
