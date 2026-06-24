function assert(condition, message) {
  if (!condition) throw new Error(message);
}

let observedBeforeImport = "unset";
readBeforeImport();

import { value } from "./static_dependency_order_dep.mjs";

function readBeforeImport() {
  observedBeforeImport = value;
}

assert(
  observedBeforeImport === "dependency-ready",
  `static dependency should initialize before module body, got ${observedBeforeImport}`
);

console.log("test_module_static_dependency_order: OK");

