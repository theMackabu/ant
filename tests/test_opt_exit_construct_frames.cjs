const assert = require('node:assert');

function leaf(i, at) { if (i === at) return new Error('x').stack; return 0; }
function Plain(n) { let s = 0; for (let i = 0; i < n; i++) { const r = leaf(i, n - 2); if (r) this.r = r; s += i; } this.s = s; }
function Guarded(n) { for (let i = 0; i < n; i++) { try { const r = leaf(i, n - 2); if (r) this.r = r; } catch (e) {} } }
class Klass { constructor(n) { for (let i = 0; i < n; i++) { try { const r = leaf(i, n - 2); if (r) this.r = r; } catch (e) {} } } }
function calls(n) { let r; for (let i = 0; i < n; i++) { const s = leaf(i, n - 2); if (s) r = s; } return r; }

const caller = (stack) => /^\s*at (new )?(\w+)/.exec(String(stack).split('\n')[2]).slice(1, 3).join('');
for (let round = 0; round < 4; round++) {
  assert.strictEqual(caller(new Plain(3000).r), 'new Plain');
  assert.strictEqual(caller(new Guarded(3000).r), 'new Guarded');
  assert.strictEqual(caller(new Klass(3000).r), 'new Klass');
  assert.strictEqual(caller(calls(3000)), 'calls');
}

console.log('ok');
