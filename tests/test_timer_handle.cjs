// Timeout and Interval handles keep their id on the prototype's
// Symbol.toPrimitive, inspect with Node's refed/destroyed state, and stay
// usable after they fire or are cleared. Handles that are dropped get collected without disturbing
// timers that are still pending.
const assert = require('node:assert');
const { inspect } = require('node:util');

const t = setTimeout(() => {}, 1000);
const iv = setInterval(() => {}, 1000);
const im = setImmediate(() => {});

const state = (refed, destroyed) => `  Symbol(refed): ${refed},\n  Symbol(destroyed): ${destroyed}\n}`;
assert.strictEqual(inspect(t), `Timeout (${+t}) {\n  delay: 1000,\n  repeat: null,\n${state(true, false)}`);
assert.strictEqual(inspect(iv), `Interval (${+iv}) {\n  delay: 1000,\n  repeat: 1000,\n${state(true, false)}`);
assert.strictEqual(inspect(im), `Immediate {\n${state(true, false)}`);
assert.strictEqual(inspect(setImmediate(() => {}).unref()), `Immediate {\n${state(false, false)}`);
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
assert.ok(inspect(t).endsWith(state(true, true)));
assert.ok(inspect(im).endsWith(state(null, true)));

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
    console.log('PASS timer handles inspect like Node, keep their id and are collectable');
  }, 40);
}, 10);
