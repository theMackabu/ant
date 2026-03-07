import * as esmNs from "./cjs_esm_circular_entry.mjs";
import cjsDefault from "./cjs_esm_circular_bridge.cjs";
import * as cjsNs from "./cjs_esm_circular_bridge.cjs";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(esmNs.esmValue === "esm-value", "esm circular export mismatch");
assert(esmNs.fromCjs === "cjs-value", "esm should read cjs export in circular edge");

assert(cjsDefault.cjsValue === "cjs-value", "cjs default export mismatch");
assert(cjsDefault.esmValueSeen === "esm-value", "cjs getter should see resolved esm export");

assert(cjsNs.default === cjsDefault, "namespace.default should match cjs default import");
assert(
  cjsNs.esmValueSeen === "esm-value" || cjsNs.esmValueSeen === undefined,
  "cjs named accessor export should be stable under circular loading"
);

console.log("test_cjs_esm_circular: OK");
