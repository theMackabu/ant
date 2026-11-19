const assert = require('node:assert');

function evalLoop(n) { return eval('var z = 0; for (let i = 0; i < n; i++) { z++; } z'); }
function evalVarThenLoop(n) { eval('var w = 0;'); for (let i = 0; i < n; i++) { w++; } return w; }
function evalLexical(n) { return eval('let q = 0; for (let i = 0; i < n; i++) { q++; } q'); }
function evalWrites(n) { let total = 0; eval('for (let i = 0; i < n; i++) { total += i; }'); return total; }
function evalNested(n) { return eval('eval("var y = 0; for (let i = 0; i < n; i++) y += 2; y")'); }
for (let i = 0; i < 3; i++) {
  assert.strictEqual(evalLoop(400), 400);
  assert.strictEqual(evalLoop(600), 600);
  assert.strictEqual(evalLoop(5000), 5000);
  assert.strictEqual(evalVarThenLoop(3000), 3000);
  assert.strictEqual(evalLexical(3000), 3000);
  assert.strictEqual(evalWrites(3000), 4498500);
  assert.strictEqual(evalNested(3000), 6000);
}
assert.strictEqual((0, eval)('var top1 = 0; for (let i = 0; i < 3000; i++) { top1++; } top1'), 3000);

console.log('ok');
