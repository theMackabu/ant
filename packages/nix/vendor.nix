{ lib, stdenvNoCC, meson, ninja, git, cacert, gitRev ? "unknown" }:

let
  src = lib.fileset.toSource {
    root = ../..;
    fileset = lib.fileset.unions [
      ../../meson.build
      ../../meson_options.txt
      ../../meson
      (lib.fileset.fileFilter (f: f.hasExt "wrap") ../../vendor)
      ../../vendor/packagefiles
    ];
  };
in
stdenvNoCC.mkDerivation {
  pname = "ant-vendor";
  version = "cache";

  inherit src;
  nativeBuildInputs = [ meson ninja git cacert ];

  dontConfigure = true;
  dontPatchShebangs = true;
  dontFixup = true;

  buildPhase = ''
    runHook preBuild
    export HOME=$TMPDIR
    meson subprojects download || true
    test -n "$(ls -A vendor/packagecache 2>/dev/null)" \
      || test -d vendor/boringssl \
      || { echo "FATAL: meson subprojects download produced nothing"; exit 1; }
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    find vendor -type d -name .git -prune -exec rm -rf {} +
    rm -rf vendor/packagecache
    mkdir -p $out
    cp -r vendor/. $out/
    runHook postInstall
  '';

  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
  outputHash = "sha256-75RCsk2hNEzXlnNOvCR0gTb94aXFo5LW74coTRgjyKs=";
}
