import assert from 'node:assert';

const log = globalThis.dynamicImportOrderLog = [];

let thrownSynchronously = false;
let failed;
try {
  failed = import('./fixtures/dynamic_import_order/throws.mjs');
} catch {
  thrownSynchronously = true;
}
const loaded = import('./fixtures/dynamic_import_order/dep.mjs');
const missing = import('./fixtures/dynamic_import_order/missing.mjs');
log.push('after import calls');

assert.strictEqual(thrownSynchronously, false);
assert.deepStrictEqual(log, ['after import calls']);

await assert.rejects(failed, /boom/);
await assert.rejects(missing);
const ns = await loaded;
assert.strictEqual(ns.value, 1);
assert.strictEqual(await import('./fixtures/dynamic_import_order/dep.mjs'), ns);
assert.deepStrictEqual(log, ['after import calls', 'throws body', 'dep body']);

const loadedAgain = import('./fixtures/dynamic_import_order/dep.mjs');
const loadedTwice = import('./fixtures/dynamic_import_order/dep.mjs');
assert.ok(loadedAgain instanceof Promise);
assert.notStrictEqual(loadedAgain, loadedTwice);
assert.strictEqual(await loadedAgain, ns);
assert.strictEqual(await loadedTwice, ns);
await assert.rejects(import('./fixtures/dynamic_import_order/throws.mjs'), /boom/);
assert.strictEqual(log.filter((entry) => entry === 'dep body').length, 1);
assert.strictEqual(log.filter((entry) => entry === 'throws body').length, 1);

function importInside() {
  const before = log.length;
  const pending = import('./fixtures/dynamic_import_order/nested.mjs');
  return { pending, ranInline: log.length !== before };
}
const nested = importInside();
assert.strictEqual(nested.ranInline, false);
assert.strictEqual((await nested.pending).value, 2);
assert.strictEqual(log.at(-1), 'nested body');

const coerced = import({ toString() { log.push('specifier coerced'); return './fixtures/dynamic_import_order/dep.mjs'; } });
assert.strictEqual(log.at(-1), 'specifier coerced');
assert.strictEqual(await coerced, ns);
await assert.rejects(import({ toString() { throw new RangeError('bad specifier'); } }), RangeError);
await assert.rejects(import(Symbol('specifier')), TypeError);

console.log('ok');
