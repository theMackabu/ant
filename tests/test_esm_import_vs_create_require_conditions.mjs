import importedDefault, { branch as importBranch } from "cond-exports";
import { createRequire } from "node:module";

if (typeof require !== "undefined") {
  throw new Error("ESM should not expose bare require");
}

if (globalThis.require !== undefined) {
  throw new Error("ESM should not install globalThis.require");
}

if (importBranch !== "import" || importedDefault.branch !== "import") {
  throw new Error(`ESM import should prefer exports.import, got ${importBranch}/${importedDefault.branch}`);
}

const requireFromHere = createRequire(import.meta.url);
const required = requireFromHere("cond-exports");

if (required.branch !== "require") {
  throw new Error(`createRequire should prefer exports.require, got ${required.branch}`);
}

const resolved = requireFromHere.resolve("cond-exports");
if (!/require\.cjs$/.test(resolved)) {
  throw new Error(`createRequire.resolve should prefer exports.require, got ${resolved}`);
}

console.log("esm import/createRequire conditions ok");
