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
, zig_0_15 ? null
, zig ? null
, importNpmLock
, apple-sdk_15 ? null
, callPackage
, gitRev ? "unknown"
}:

let
  zigPkg = if zig_0_15 != null then zig_0_15 else zig;

  cpuTuneFlag = "-mcpu=native";
  antVendor = callPackage ./vendor.nix { };

  toolsNodeModules = importNpmLock.buildNodeModules {
    package = lib.importJSON ../src/tools/package.json;
    packageLock = lib.importJSON ../src/tools/npm-shrinkwrap.json;
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
in

llvmPackages_21.stdenv.mkDerivation (finalAttrs: {
  pname = "ant";
  src = ../.;
  version = lib.fileContents ../meson/ant.version;

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
  ];

  buildInputs = lib.optionals stdenv.isDarwin [ apple-sdk_15 ];

  postUnpack = ''
    chmod -R u+w "$sourceRoot/vendor"
    cp -rT --no-preserve=mode ${antVendor} "$sourceRoot/vendor"
    chmod -R u+w "$sourceRoot/vendor"
  '';

  mesonFlags = [
    "-Dbuild_git_hash=${gitRev}"
    "-Db_lto_mode=default"
  ];

  NIX_ENFORCE_NO_NATIVE = false;
  env.NIX_CFLAGS_COMPILE = optArgs;

  preConfigure = ''
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
    export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
    mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

    ln -sfn ${toolsNodeModules}/node_modules src/tools/node_modules
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 ant "$out/bin/ant"
    ln -s ant          "$out/bin/antx"
    runHook postInstall
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
