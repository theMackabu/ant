import {
  BinaryCache, DevShell, EachSystem, Flake, Import, Input,
  Let, Outputs, Package, path,
} from "../../ts-nix/index.ts";
import type { Expr } from "../../ts-nix/index.ts";

interface AntInputs {
  self: Expr;
  nixpkgs: Expr;
  "flake-utils": Expr;
  "rust-overlay": Expr;
}

function antOutputs(pkgs: Expr, self: Expr) {
  return new Let((scope) => {
    const toolchain = scope.bind(
      "toolchain",
      new Import("./toolchain.nix", { pkgs }),
    );

    const rustToolchain = scope.bind(
      "rustToolchain",
      pkgs
        .get("rust-bin", "fromRustupToolchainFile")
        .call(path("../../../src/temporal/rust-toolchain.toml")),
    );

    const rustPlatform = scope.bind(
      "rustPlatform",
      pkgs.get("makeRustPlatform").call({
        cargo: rustToolchain,
        rustc: rustToolchain,
      }),
    );

    const dirtyRevision = self.getOr("unknown", "dirtyShortRev");
    const ant = scope.bind(
      "ant",
      pkgs.get("callPackage").call(path("./package.nix"), {
        gitRev: self.getOr(dirtyRevision, "shortRev"),
        stdenv: toolchain.get("stdenv"),
        rustPlatform,
        rustToolchain,
      }),
    );

    const shell = new Import("./shell.nix", { pkgs, toolchain });

    return new Outputs(
      new Package("ant", ant).asDefault(),
      new DevShell("default", shell),
    );
  });
}

function systemOutputs(inputs: AntInputs, system: Expr) {
  return new Let((scope) => {
    const pkgs = scope.bind(
      "pkgs",
      new Import(inputs.nixpkgs, {
        system,
        overlays: [inputs["rust-overlay"].get("overlays", "default")],
      }),
    );

    return antOutputs(pkgs, inputs.self);
  });
}

function flakeOutputs(inputs: AntInputs) {
  return new EachSystem(inputs["flake-utils"], (system) =>
    systemOutputs(inputs, system),
  );
}

const nixpkgs = new Input("nixpkgs", "github:NixOS/nixpkgs/nixos-unstable");
const flakeUtils = new Input("flake-utils", "github:numtide/flake-utils");

const rustOverlay = new Input(
  "rust-overlay",
  "github:oxalica/rust-overlay",
).follows("nixpkgs", nixpkgs);

const cache = new BinaryCache(
  "https://ant.cachix.org",
  "ant.cachix.org-1:v/FbrMBfZ/rZHKZtAqM5mpLu6YKLaDF64dcLP30VTH0=",
);

export const project = new Flake(
  "javascript for 🐜's, a tiny runtime with big ambitions",
)
  .inputs(nixpkgs, flakeUtils, rustOverlay)
  .cache(cache);

export default project.outputs(flakeOutputs);

