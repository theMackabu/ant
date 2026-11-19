import assert from 'node:assert';

const log = globalThis.dynamicImportTlaLog = [];
const fixture = (name) => new URL(`./fixtures/dynamic_import_tla/${name}`, import.meta.url).href;

const first = import(fixture('middle.mjs'));
const second = import(fixture('middle.mjs'));
const direct = import(fixture('slow.mjs'));
const [a, b, slow] = await Promise.all([first, second, direct]);

assert.strictEqual(a, b);
assert.strictEqual(a.fromMiddle, 'ready');
assert.strictEqual(slow.value, 'ready');
assert.ok(a.url.endsWith('middle.mjs'));
assert.deepStrictEqual(log, ['slow start', 'slow end', 'middle sees ready']);

const outer = await import(fixture('outer.mjs'));
assert.strictEqual(outer.combined, 'ready!');
assert.strictEqual(outer.afterOwnAwait, 'ready!?');

await assert.rejects(import(fixture('needs_failing.mjs')), RangeError);
await assert.rejects(import(fixture('needs_failing.mjs')), RangeError);
assert.ok(!log.includes('needs_failing body ran'));

const cycle = await import(fixture('cycle_a.mjs'));
assert.strictEqual(cycle.name, 'A');
assert.strictEqual(cycle.fromB(), 'B');
assert.deepStrictEqual(log.filter((entry) => entry.startsWith('cycle')), ['cycle b after tick', 'cycle a after tick']);

for (let attempt = 0; attempt < 3; attempt++) await assert.rejects(import(fixture('rejects_later.mjs')), RangeError);
await assert.rejects(import(fixture('needs_rejected.mjs')), RangeError);
assert.strictEqual(log.filter((entry) => entry === 'rejects_later body').length, 1);
assert.ok(!log.includes('needs_rejected body ran'));

const { stack } = await import(fixture('deferred_stack.mjs'));
assert.ok(stack.includes('deferred_stack.mjs'));
assert.ok(!stack.split('\n')[1].includes('test_dynamic_import_tla_dependency'));

const again = await import(fixture('middle.mjs'));
assert.strictEqual(again, a);

console.log('ok');
