// Timeout and Interval handles keep their id on the prototype's
// Symbol.toPrimitive, inspect compactly, and stay usable after they fire or
// are cleared. Handles that are dropped get collected without disturbing
// timers that are still pending.
const assert = require('node:assert');
const { inspect } = require('node:util');

const t = setTimeout(() => {}, 1000);
const iv = setInterval(() => {}, 1000);
const im = setImmediate(() => {});

assert.strictEqual(inspect(t), `Timeout (${+t}) { delay: 1000, repeat: null }`);
assert.strictEqual(inspect(iv), `Interval (${+iv}) { delay: 1000, repeat: 1000 }`);
assert.strictEqual(inspect(im), 'Immediate {}');
assert.deepStrictEqual(Object.getOwnPropertySymbols(t), []);
assert.strictEqual(typeof Object.getPrototypeOf(t)[Symbol.toPrimitive], 'function');
assert.ok(Number.isInteger(+t) && +iv !== +t);

const id = +t;
clearImmediate(im);
clearTimeout(im);
assert.strictEqual(t.hasRef(), true, 'clearTimeout(immediate) must not clear a timer');
clearTimeout(id);
clearInterval(iv);
assert.strictEqual(t.hasRef(), false);
assert.strictEqual(+t, id, 'id survives clearing');

const out = [];
const a = setTimeout((x, y) => out.push([x, y]), 1, 'a', 'b');

for (let i = 0; i < 20000; i++) setTimeout(() => {}, 0);
const keep = [];
for (let i = 0; i < 200; i++) keep.push(setTimeout(() => out.push('kept'), 30));
for (let i = 0; i < 100; i++) clearTimeout(keep[i * 2]);

setTimeout(() => {
  if (typeof gc === 'function') gc();
  a.refresh();
  setTimeout(() => {
    assert.deepStrictEqual(out.filter(x => x === 'kept').length, 100);
    assert.deepStrictEqual(out.filter(Array.isArray), [['a', 'b'], ['a', 'b']]);
    console.log('PASS timer handles are compact, stable and collectable');
  }, 40);
}, 10);
