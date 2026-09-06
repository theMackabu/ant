import { AttrSet, Let, NixFunction, ifElse, nix, path, ref, str } from "../../ts-nix/index.ts";
import type { Expr } from "../../ts-nix/index.ts";

function parseLine(lib: Expr) {
  return new NixFunction("line", (line) => new Let((scope) => {
    const parts = scope.bind("parts", lib.get("splitString").call("=", line));
    const field = (index: number) => lib.get("trim").call(lib.get("elemAt").call(parts, index));

    return new AttrSet({ name: field(0), value: field(1) });
  }));
}

function isVersionLine(lib: Expr) {
  return new NixFunction("line", (line) => new Let((scope) => {
    const trimmed = scope.bind("trimmed", lib.get("trim").call(line));
    return nix`${trimmed} != "" && !(${lib.get("hasPrefix").call("#", trimmed)})`;
  }));
}

export default new NixFunction(["lib", "gitRev"], ({ lib, gitRev }) => {
  return new Let((scope) => {
    const builtins = ref("builtins");
    const contents = lib.get("fileContents").call(path("../../../meson/ant.version"));
    const lines = lib.get("splitString").call("\n", contents);
    const filtered = builtins.get("filter").call(isVersionLine(lib), lines);
    const fields = builtins.get("map").call(parseLine(lib), filtered);
    const version = scope.bind("version", builtins.get("listToAttrs").call(fields));

    const revision = scope.bind("revision", ifElse(
      gitRev.equals("unknown"),
      gitRev,
      builtins.get("substring").call(0, 8, gitRev),
    ));

    return str`${version.get("major")}.${version.get("minor")}.${revision}.${version.get("patch")}`;
  });
}, { gitRev: "unknown" });
