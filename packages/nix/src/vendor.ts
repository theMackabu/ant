import { AttrSet, Let, NixFunction, path, str } from "../../ts-nix/index.ts";

const buildPhase = `runHook preBuild
export HOME=$TMPDIR
meson subprojects download || true
test -n "$(ls -A vendor/packagecache 2>/dev/null)" \\
  || test -d vendor/boringssl \\
  || { echo "FATAL: meson subprojects download produced nothing"; exit 1; }
runHook postBuild
`;

const installPhase = `runHook preInstall
find vendor -type d -name .git -prune -exec rm -rf {} +
rm -rf vendor/packagecache
mkdir -p $out
cp -r vendor/. $out/
runHook postInstall
`;

export default new NixFunction(
  ["lib", "stdenvNoCC", "meson", "ninja", "git", "cacert"],
  ({ lib, stdenvNoCC, meson, ninja, git, cacert }) => new Let((scope) => {
    const fileset = lib.get("fileset");
    const wraps = fileset.get("fileFilter").call(
      new NixFunction("file", (file) => file.get("hasExt").call("wrap")),
      path("../../../vendor"),
    );

    const src = scope.bind("src", fileset.get("toSource").call({
      root: path("../../.."),
      fileset: fileset.get("unions").call([
        path("../../../meson.build"),
        path("../../../meson_options.txt"),
        path("../../../meson"),
        wraps,
        path("../../../vendor/packagefiles"),
      ]),
    }));

    return stdenvNoCC.get("mkDerivation").call(new AttrSet({
      pname: "ant-vendor",
      version: str`cache-${stdenvNoCC.get("hostPlatform", "system")}`,
      src,
      nativeBuildInputs: [meson, ninja, git, cacert],
      dontConfigure: true,
      dontPatchShebangs: true,
      dontFixup: true,
      buildPhase,
      installPhase,
      outputHashMode: "recursive",
      outputHashAlgo: "sha256",
      outputHash: "sha256-DjbXYLyqCh3NZr4Iv+ymAg81xe40zfujBRYSzym1bCQ=",
    }));
  }),
);
