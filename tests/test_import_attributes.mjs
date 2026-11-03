import assert from 'node:assert';

const dir = new URL('./fixtures/import-attributes/', import.meta.url);
const at = (name) => new URL(name, dir).href;

const { default: loaded } = await import(at('static_json.mjs'));
assert.deepStrictEqual(loaded.data, { name: 'ant', list: [1, 2, 3] });
assert.strictEqual(loaded.same, true);
assert.strictEqual(loaded.value, 42);

const reexport = await import(at('reexport.mjs'));
assert.strictEqual(reexport.data.name, 'ant');
assert.strictEqual(reexport.ns.default.name, 'ant');

const dynamic = await import(at('data.json'), { with: { type: 'json' } });
assert.strictEqual(dynamic.default.name, 'ant');
assert.strictEqual((await import(at('plain.mjs'), { with: {} })).value, 42);

await assert.rejects(import(at('plain.mjs'), { with: { type: 'json' } }), TypeError);
await assert.rejects(import(at('data.json'), { with: { type: 'css' } }), TypeError);
await assert.rejects(import('node:fs', { with: { type: 'json' } }), TypeError);
await assert.rejects(import(at('data.json'), { with: { type: 1 } }), TypeError);
await assert.rejects(import(at('data.json'), { with: 1 }), TypeError);

await assert.rejects(import(at('static_mismatch.mjs')), TypeError);
await assert.rejects(import(at('static_unknown_type.mjs')), TypeError);
await assert.rejects(import(at('reexport_mismatch.mjs')), TypeError);
if (typeof Ant !== 'undefined') await assert.rejects(import(at('static_unknown_key.mjs')), SyntaxError);
await assert.rejects(import(at('static_duplicate.mjs')), SyntaxError);
await assert.rejects(import(at('static_nonstring.mjs')), SyntaxError);

console.log('ok');
