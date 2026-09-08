const assert = require('node:assert');

function makeCalls(replacement) {
  const local = 41;
  const calls = {
    dynamic(source, extra) { return eval(source, extra()); },
    literal() { return eval('local + 1'); },
    nonString() { return eval(7); },
    empty() { return eval(); },
    set(value) { eval = value; }
  };
  eval('var eval = replacement;');
  return calls;
}

const received = [];
const rebound = makeCalls(function (...args) {
  received.push(args);
  return 'rebound';
});
let effects = 0;
assert.strictEqual(rebound.dynamic('throw new Error("must not execute")', () => ++effects), 'rebound');
assert.strictEqual(effects, 1);
assert.strictEqual(rebound.literal(), 'rebound');
assert.strictEqual(rebound.nonString(), 'rebound');
assert.strictEqual(rebound.empty(), 'rebound');
assert.deepStrictEqual(received, [
  ['throw new Error("must not execute")', 1], ['local + 1'], [7], []
]);

// Saving the callee must precede argument evaluation, even if it rebinds eval.
assert.strictEqual(rebound.dynamic('ignored', () => rebound.set(null)), 'rebound');
assert.throws(() => rebound.empty(), TypeError);

const direct = makeCalls(globalThis.eval);
assert.strictEqual(direct.literal(), 42);
assert.strictEqual(direct.dynamic('local + 1', () => ++effects), 42);
assert.strictEqual(effects, 2);
assert.strictEqual(direct.nonString(), 7);
assert.strictEqual(direct.empty(), undefined);

console.log('dynamic eval binding tests passed');
