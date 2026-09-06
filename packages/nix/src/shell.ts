import { AttrSet, Let, NixFunction, ifElse, str } from "../../ts-nix/index.ts";

export default new NixFunction(["pkgs", "toolchain"], ({ pkgs, toolchain }) => {
  return new Let((scope) => {
    const lib = pkgs.get("lib");
    const platform = pkgs.get("stdenv", "hostPlatform");
    const nativeTuneFlag = ifElse(platform.get("isx86"), "-march=native", "-mcpu=native");

    const optArgs = scope.bind("optArgs", lib.get("concatStringsSep").call(" ", [
      nativeTuneFlag,
      "-Qunused-arguments",
      "-fvisibility=hidden",
      "-fvisibility-inlines-hidden",
      "-fno-math-errno",
      "-fno-trapping-math",
      "-fno-stack-protector",
      "-mllvm",
      "-enable-machine-outliner=never",
    ]));

    const environment = new AttrSet({
      packages: [
        pkgs.get("nodejs_22"),
        ...["bintools", "clang", "compilerRt", "llvm"].map((name) => toolchain.get(name)),
      ],
      CFLAGS: optArgs,
      CXXFLAGS: optArgs,
      NIX_CFLAGS_COMPILE: optArgs,
      NIX_ENFORCE_NO_NATIVE: "0",
      LDFLAGS: str`-resource-dir=${toolchain.get("compilerRt")}`,
      CC: str`${toolchain.get("clang")}/bin/clang`,
      CXX: str`${toolchain.get("clang")}/bin/clang++`,
    });

    const darwin = lib.get("optionalAttrs").call(platform.get("isDarwin"), {
      LD: str`${toolchain.get("bintools")}/bin/ld`,
      AR: str`${toolchain.get("bintools")}/bin/ar`,
      RANLIB: str`${toolchain.get("bintools")}/bin/ranlib`,
      STRIP: str`${toolchain.get("bintools")}/bin/strip`,
      shellHook: 'export SDKROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk"\n',
    });

    return pkgs.get("mkShellNoCC").call(environment.merge(darwin));
  });
});
