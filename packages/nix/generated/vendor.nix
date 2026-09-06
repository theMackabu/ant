# Generated from packages/nix/src/vendor.ts; run ant packages/nix/generate.ts.
({ lib, stdenvNoCC, meson, ninja, git, cacert }: (let
  __tsnix_0_lib = lib;
  __tsnix_1_stdenvNoCC = stdenvNoCC;
  __tsnix_2_meson = meson;
  __tsnix_3_ninja = ninja;
  __tsnix_4_git = git;
  __tsnix_5_cacert = cacert;
in (let
  __tsnix_7_src = ((((__tsnix_0_lib).fileset).toSource) ({
  root = ../../..;
  fileset = ((((__tsnix_0_lib).fileset).unions) ([ (../../../meson.build) (../../../meson_options.txt) (../../../meson) (((((__tsnix_0_lib).fileset).fileFilter) ((__tsnix_6_file: (((__tsnix_6_file).hasExt) ("wrap")))) (../../../vendor))) (../../../vendor/packagefiles) ]));
}));
in (((__tsnix_1_stdenvNoCC).mkDerivation) ({
  pname = "ant-vendor";
  version = "cache-${(__tsnix_1_stdenvNoCC).hostPlatform.system}";
  src = __tsnix_7_src;
  nativeBuildInputs = [ (__tsnix_2_meson) (__tsnix_3_ninja) (__tsnix_4_git) (__tsnix_5_cacert) ];
  dontConfigure = true;
  dontPatchShebangs = true;
  dontFixup = true;
  buildPhase = "runHook preBuild\nexport HOME=$TMPDIR\nmeson subprojects download || true\ntest -n \"$(ls -A vendor/packagecache 2>/dev/null)\" \\\n  || test -d vendor/boringssl \\\n  || { echo \"FATAL: meson subprojects download produced nothing\"; exit 1; }\nrunHook postBuild\n";
  installPhase = "runHook preInstall\nfind vendor -type d -name .git -prune -exec rm -rf {} +\nrm -rf vendor/packagecache\nmkdir -p $out\ncp -r vendor/. $out/\nrunHook postInstall\n";
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
  outputHash = "sha256-DjbXYLyqCh3NZr4Iv+ymAg81xe40zfujBRYSzym1bCQ=";
})))))
