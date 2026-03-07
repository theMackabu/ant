import * as ns from "./cjs_accessor_module.cjs";
import def from "./cjs_accessor_module.cjs";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(ns.answer === 42, "namespace accessor export should resolve to 42");
assert(ns.getterCalls >= 1, "accessor getter should have been invoked");

assert(def.answer === 42, "default CJS export accessor should resolve to 42");
assert(ns.default === def, "namespace.default should match default import");

console.log("test_cjs_accessor_exports: OK");
