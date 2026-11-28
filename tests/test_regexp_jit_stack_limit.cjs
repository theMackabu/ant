const assert = require("node:assert");

// Long subjects must not overflow PCRE2's JIT stack, which reports no match
// (the old 512 KB stack failed at ~100000 chars / ~35000 astral code points).
const G = "\u{1F600}";
for (const s of ["x".repeat(300000), G.repeat(60000)]) {
  assert.strictEqual(/^(?:.)*$/.test(s), true);
  assert.strictEqual(/^(?:.|x)*$/.test(s), true);
  assert.strictEqual(s.replace(/(?:[^])*/, "").length, 0);
  assert.strictEqual(/^(?:.)*$/su.test(s + "y"), true);
}

console.log("ok");
