import * as root from "./module_eval_reentrant_root.mjs";
import * as mid from "./module_eval_reentrant_mid.mjs";
import * as leaf from "./module_eval_reentrant_leaf.mjs";

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(root.root === "root-ok", "root export mismatch");
assert(root.fromMid === "mid-ok", "root->mid export mismatch");
assert(root.fromLeaf === "leaf-ok", "root->leaf export mismatch");

assert(mid.mid === "mid-ok", "mid export mismatch");
assert(mid.fromLeaf === "leaf-ok", "mid->leaf export mismatch");

assert(leaf.leaf === "leaf-ok", "leaf export mismatch");

assert(root.mid === undefined, "root namespace was polluted by mid exports");
assert(mid.root === undefined, "mid namespace was polluted by root exports");
assert(leaf.root === undefined, "leaf namespace was polluted by root exports");

console.log("test_module_eval_reentrancy: OK");
