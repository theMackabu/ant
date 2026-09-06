import {
  AttrSet, Import, Let, NixFunction, ifElse, nix, path, ref, str,
} from "../../ts-nix/index.ts";
import type { Expr, Scope } from "../../ts-nix/index.ts";

const parameters = [
  "lib", "llvmPackages_21", "stdenv", "ccache",
  "meson", "ninja", "cmake", "pkg-config", "python3", "nodejs_22", "git", "curl",
  "zig_0_16", "zig", "importNpmLock", "overrideCC", "rustPlatform", "rustToolchain",
  "runCommand", "writeText", "darwin", "callPackage", "gitRev", "enablePgo", "enableNativeTuning",
] as const;

type Dependencies = { [Name in typeof parameters[number]]: Expr };

const optimizationFlags = [
  "-Qunused-arguments",
  "-fvisibility=hidden",
  "-fvisibility-inlines-hidden",
  "-fno-math-errno",
  "-fno-trapping-math",
  "-fno-stack-protector",
  "-mllvm",
  "-enable-machine-outliner=never",
];

const ccacheConfig = `export CCACHE_COMPRESS=1
export CCACHE_MAXSIZE=2G
export CCACHE_SLOPPINESS=random_seed,time_macros
if [ -d /tmp/ant-nix-ccache ] && [ -w /tmp/ant-nix-ccache ]; then
  export CCACHE_DIR=/tmp/ant-nix-ccache
else
  export CCACHE_DIR="$TMPDIR/ccache"
fi
`;

function compiler(deps: Dependencies, scope: Scope) {
  const { stdenv, llvmPackages_21: llvm, overrideCC, ccache } = deps;
  const linuxCompiler = llvm.get("stdenv", "cc", "override").call({
    bintools: llvm.get("bintools"),
  });

  const base = scope.bind("antBaseStdenv", ifElse(
    stdenv.get("hostPlatform", "isLinux"),
    overrideCC.call(llvm.get("stdenv"), linuxCompiler),
    stdenv,
  ));

  const links = ccache.get("links").call({
    unwrappedCC: base.get("cc", "cc"),
    extraConfig: ccacheConfig,
  });

  const cached = scope.bind("ccacheLinks", links.get("overrideAttrs").call(
    new NixFunction("previous", (previous) => ({
      passthru: previous.getOr({}, "passthru").merge({
        langC: base.get("cc", "cc").getOr(true, "langC"),
        langCC: base.get("cc", "cc").getOr(true, "langCC"),
      }),
    })),
  ));

  return scope.bind("antStdenv", overrideCC.call(
    base,
    base.get("cc", "override").call({ cc: cached }),
  ));
}

function cargoDependencies(deps: Dependencies, scope: Scope) {
  const fetch = deps.rustPlatform.get("fetchCargoVendor");
  const temporal = scope.bind("temporalCargoDeps", fetch.call({
    src: path("../../.."),
    cargoRoot: "src/temporal",
    name: "ant-temporal-cargo-deps",
    hash: "sha256-7Ny7Y3VdOB5GFS2SoUUhqIzTJsVRdjsPTon9ndKm5RA=",
  }));

  const standardLibrary = scope.bind("rustStdCargoDeps", fetch.call({
    src: deps.rustToolchain,
    cargoRoot: "lib/rustlib/src/rust/library",
    name: "ant-rust-std-cargo-deps",
    hash: "sha256-kUUC6D6xrFap7+gn+lq1i3lBawfzbnUmDfB5QIvLnYA=",
  }));

  return scope.bind("temporalBuildCargoDeps", deps.runCommand.call(
    "ant-temporal-build-cargo-deps",
    {},
    str`mkdir -p "$out"
cp -R ${standardLibrary}/. "$out/"
chmod -R u+w "$out"
cp -R ${temporal}/. "$out/"
`,
  ));
}

function pgoConfiguration(deps: Dependencies, scope: Scope) {
  const platform = deps.stdenv.get("hostPlatform", "parsed");
  const filename = scope.bind("pgoFileName", str`ant-${platform.get("kernel", "name")}-${platform.get("cpu", "name")}.profdata`);
  const profile = nix`${path("../../../meson/pgo/profiles")} + ${str`/${filename}`}`;
  const exists = ref("builtins").get("pathExists").call(profile);
  const missing = str`enablePgo requested but missing PGO profile: meson/pgo/profiles/${filename}`;

  const flags = ifElse(
    deps.enablePgo,
    ifElse(exists, ["-Dpgo=enabled"], ref("throw").call(missing)),
    ["-Dpgo=disabled"],
  );

  return { filename, flags };
}

function buildEnvironment(deps: Dependencies, stdenv: Expr, optArgs: Expr) {
  const target = deps.stdenv.get("hostPlatform", "rust", "cargoEnvVarTarget");
  const clang = str`${stdenv.get("cc")}/bin/clang`;
  const clangxx = str`${stdenv.get("cc")}/bin/clang++`;
  const executable = deps.lib.get("getExe'");

  return new AttrSet({
    ANT_TEMPORAL_CARGO: executable.call(deps.rustToolchain, "cargo"),
    RUSTC: executable.call(deps.rustToolchain, "rustc"),
    HOST_CC: clang,
    HOST_CXX: clangxx,
    NIX_CFLAGS_COMPILE: optArgs,
  })
    .set(str`CARGO_TARGET_${target}_LINKER`, clang)
    .set(str`CC_${target}`, clang)
    .set(str`CXX_${target}`, clangxx);
}

function buildPackage(deps: Dependencies) {
  return new Let((scope) => {
    const { lib, gitRev, rustToolchain, enablePgo, enableNativeTuning } = deps;
    const stdenv = compiler(deps, scope);
    const cargoDeps = cargoDependencies(deps, scope);
    const pgo = pgoConfiguration(deps, scope);
    const isDarwin = deps.stdenv.get("hostPlatform", "isDarwin");
    const optionalAttrs = lib.get("optionalAttrs");
    const optionals = lib.get("optionals");
    const optionalString = lib.get("optionalString");

    const vendor = scope.bind("antVendor", deps.callPackage.call(path("./vendor.nix"), {}));
    const nativeFile = scope.bind("mesonNativeFile", deps.writeText.call(
      "ant-meson-native.ini",
      str`[binaries]
c = '${stdenv.get("cc")}/bin/clang'
cpp = '${stdenv.get("cc")}/bin/clang++'
`,
    ));

    const tools = scope.bind("toolsNodeModules", deps.importNpmLock.get("buildNodeModules").call({
      package: lib.get("importJSON").call(path("../../../src/tools/package.json")),
      packageLock: lib.get("importJSON").call(path("../../../src/tools/npm-shrinkwrap.json")),
      nodejs: deps.nodejs_22,
    }));

    const optArgs = lib.get("concatStringsSep").call(" ", optimizationFlags);
    const zig = ifElse(nix`${deps.zig_0_16} != null`, deps.zig_0_16, deps.zig);
    const nativeInputs = nix`${[
      deps.meson, deps.ninja, deps.cmake, deps["pkg-config"], deps.python3,
      deps.nodejs_22, deps.git, deps.curl, zig, deps.rustPlatform.get("cargoSetupHook"),
    ]} ++ ${optionals.call(isDarwin, [deps.darwin.get("sigtool"), deps.llvmPackages_21.get("llvm")])}`;

    const mesonFlags = nix`${[
      str`--native-file=${nativeFile}`,
      str`-Dbuild_git_hash=${gitRev}`,
      "-Db_lto_mode=default",
      "-Dembed_example=disabled",
    ]} ++ ${optionals.call(isDarwin, [
      str`-Dllvm_nm=${lib.get("getExe'").call(deps.llvmPackages_21.get("llvm"), "llvm-nm")}`,
    ])} ++ ${optionals.call(enableNativeTuning, ["-Dnative_tuning=enabled"])} ++ ${pgo.flags}`;

    const preConfigure = nix`${str`export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache
export ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-local-cache
mkdir -p "$ZIG_GLOBAL_CACHE_DIR" "$ZIG_LOCAL_CACHE_DIR"

ln -sfn ${tools}/node_modules src/tools/node_modules
`} + ${optionalString.call(enablePgo, str`echo "==> PGO profile available: meson/pgo/profiles/${pgo.filename}"
`)}`;

    const attrs = new AttrSet({
      pname: "ant",
      src: path("../../.."),
      version: new Import("./version.nix", { lib, gitRev }),
      nativeBuildInputs: nativeInputs,
      cargoDeps,
      cargoRoot: "src/temporal",
      postUnpack: str`chmod -R u+w "$sourceRoot/vendor"
cp -rT --no-preserve=mode ${vendor} "$sourceRoot/vendor"
chmod -R u+w "$sourceRoot/vendor"
`,
      mesonFlags,
      env: buildEnvironment(deps, stdenv, optArgs),
      preConfigure,
      installPhase: `runHook preInstall
install -Dm755 ant "$out/bin/ant"
ln -s ant "$out/bin/antx"
runHook postInstall
`,
      postFixup: optionalString.call(isDarwin, str`strip -S -x "$out/bin/ant"
codesign --force --sign - --entitlements ${path("../../../meson/ant.entitlements")} "$out/bin/ant"
`),
      doCheck: false,
      meta: {
        description: "Ant JavaScript runtime",
        homepage: "https://github.com/themackabu/ant",
        license: lib.get("licenses", "mit"),
        platforms: lib.get("platforms", "unix"),
        mainProgram: "ant",
      },
    });

    const tuning = optionalAttrs.call(enableNativeTuning, { NIX_ENFORCE_NO_NATIVE: false });
    return stdenv.get("mkDerivation").call(
      new NixFunction("finalAttrs", () => attrs.merge(tuning)),
    );
  });
}

export default new NixFunction(parameters, buildPackage, {
  zig_0_16: null,
  zig: null,
  darwin: null,
  gitRev: "unknown",
  enablePgo: false,
  enableNativeTuning: false,
});
