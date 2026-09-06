import { AttrSet, NixFunction, ifElse } from "../../ts-nix/index.ts";
import type { Expr } from "../../ts-nix/index.ts";

function llvmToolchain(pkgs: Expr) {
  const llvm = pkgs.get("llvmPackages_21");
  const bintools = ifElse(
    pkgs.get("stdenv", "hostPlatform", "isDarwin"),
    pkgs.get("darwin", "binutils-unwrapped"),
    llvm.get("bintools"),
  );

  return new AttrSet()
    .set("clang", llvm.get("clang-unwrapped", "out"))
    .set("compilerRt", llvm.get("compiler-rt"))
    .set("llvm", llvm.get("llvm"))
    .set("bintools", bintools)
    .set("stdenv", llvm.get("stdenv"));
}


export default new NixFunction(["pkgs"], ({ pkgs }) => llvmToolchain(pkgs));
