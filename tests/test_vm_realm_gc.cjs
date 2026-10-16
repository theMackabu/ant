const assert = require('node:assert');
const vm = require('node:vm');

const count = process.env.ANT_GC_STRESS ? 60 : 2000;
const keptContexts = [];
let keptClosure;
let keptBuiltin;

const before = process.memoryUsage().rss;
for (let i = 0; i < count; i++) {
  const context = vm.createContext({ i });
  assert.strictEqual(vm.runInContext('i * 2', context), i * 2);
  if (i % 500 === 0) keptContexts.push(context);
  if (i === 5) keptClosure = vm.runInContext('(x) => [x, i, Array.isArray([]), new Map([[1, i]]).get(1)]', context);
  if (i === 7) keptBuiltin = vm.runInContext('Array.prototype.push', context);
}

const script = new vm.Script('globalThis.k = (globalThis.k || 0) + 1; [1, 2].map((x) => x * k)');
for (let i = 0; i < count; i++) assert.deepStrictEqual(script.runInNewContext({}), [1, 2]);

keptContexts.forEach((context, index) => {
  assert.strictEqual(vm.runInContext('i', context), index * 500);
  assert.strictEqual(vm.runInContext('typeof Array.from', context), 'function');
});
assert.deepStrictEqual(keptClosure(1), [1, 5, true, 5]);
const target = [];
assert.strictEqual(keptBuiltin.call(target, 'x'), 1);
assert.deepStrictEqual(target, ['x']);

if (!process.env.ANT_GC_STRESS) {
  const grown = process.memoryUsage().rss - before;
  assert.ok(grown < 400 * 1024 * 1024, `rss grew ${grown} bytes`);
}

console.log('vm realm gc: ok');
