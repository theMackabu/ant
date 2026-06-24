import { setPrototypeOf } from "./destructured_export_clause_source.mjs";

if (typeof setPrototypeOf !== "function") {
  throw new Error(`expected destructured export clause to publish function, got ${typeof setPrototypeOf}`);
}

const target = {};
setPrototypeOf(target, null);

if (Object.getPrototypeOf(target) !== null) {
  throw new Error("imported setPrototypeOf did not update prototype");
}

console.log("test_module_destructured_export_clause: OK");

