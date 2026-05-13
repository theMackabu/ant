{ lib
, llvmPackages_18
, stdenv
, meson
, ninja
, cmake
, pkg-config
, python3
, nodejs_22
, git
, curl
, rustc
, cargo
, rustPlatform
, zig_0_15 ? null
, zig ? null
, importNpmLock
, apple-sdk ? null
, callPackage
, gitRev ? "unknown"
}:

let
  zigPkg = if zig_0_15 != null then zig_0_15 else zig;

  antVendor = callPackage ./vendor.nix { };

  toolsNodeModules = importNpmLock.buildNodeModules {
    package = lib.importJSON ../src/tools/package.json;
    packageLock = lib.importJSON ../src/tools/npm-shrinkwrap.json;
    nodejs = nodejs_22;
  };
in
llvmPackages_18.stdenv.mkDerivation (finalAttrs: {
  pname = "ant";
  version = lib.fileContents ../meson/ant.version;

  src = ../.;

  nativeBuildInputs = [
    meson
    ninja
    cmake
    pkg-config
    python3
    nodejs_22
    git
    curl
    rustc
    cargo
    rustPlatform.cargoSetupHook
    zigPkg
  ];

  buildInputs = lib.optionals stdenv.isDarwin [ apple-sdk ];

  cargoDeps = rustPlatform.importCargoLock {
    lockFile = ../src/strip/Cargo.lock;
  };
  cargoRoot = "src/strip";

  postUnpack = ''
    chmod -R u+w "$sourceRoot/vendor"
    cp -rT --no-preserve=mode ${antVendor} "$sourceRoot/vendor"
    chmod -R u+w "$sourceRoot/vendor"
  '';

  postPatch = ''
    substituteInPlace meson/version/meson.build \
      --replace-fail \
        "git_hash = run_command('git', 'rev-parse', '--short', 'HEAD', check: false).stdout().strip()" \
        "git_hash = '${gitRev}'"
  '';

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
