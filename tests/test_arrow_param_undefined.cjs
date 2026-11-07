const assert = require('node:assert');

assert.strictEqual(((undefined) => undefined)(7), 7);
assert.strictEqual((undefined => undefined)(7), 7);
assert.strictEqual(((a, undefined) => undefined)(1, 7), 7);
assert.strictEqual(((undefined, b) => undefined + b)(7, 1), 8);
assert.strictEqual(((undefined = 7) => undefined)(), 7);
assert.deepStrictEqual(((...undefined) => undefined)(7), [7]);
assert.strictEqual((([undefined]) => undefined)([7]), 7);
assert.strictEqual((({ a: undefined }) => undefined)({ a: 7 }), 7);
assert.strictEqual((({ undefined }) => undefined)({ undefined: 7 }), 7);
assert.strictEqual((([{ x: [undefined = 7] }]) => undefined)([{ x: [] }]), 7);
assert.strictEqual(((undefined) => () => undefined)(7)(), 7);
assert.strictEqual(((undefined) => typeof undefined)(7), 'number');
assert.strictEqual(((globalThis) => globalThis)(7), 7);
assert.strictEqual((globalThis => globalThis)(7), 7);
assert.strictEqual(((a = undefined) => a)(), undefined);
assert.strictEqual(((a = globalThis) => a)(), globalThis);
assert.strictEqual((() => undefined)(7), undefined);
assert.strictEqual(((a) => a)(7), 7);

(async () => {
  assert.strictEqual(await (async (undefined) => undefined)(7), 7);
  assert.strictEqual(await (async undefined => undefined)(7), 7);
  assert.strictEqual(await (async () => undefined)(), undefined);
  console.log('ok');
})();
