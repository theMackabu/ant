import * as ns from "./cjs_esbuild_like_module.cjs";
import def from "./cjs_esbuild_like_module.cjs";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof ns.build === "function", "missing named build export");
assert(typeof ns.transform === "function", "missing named transform export");
assert(ns.version === "0.0-test", "missing named version export");

const buildResult = ns.build({ minify: false });
assert(buildResult && buildResult.ok === true, "named build export returned wrong value");
assert(ns.transform("ab") === "AB", "named transform export returned wrong value");

assert(def.build === ns.build, "default and named build exports diverged");
assert(ns.default === def, "namespace.default should match default import");

console.log("test_cjs_esbuild_named_exports: OK");
