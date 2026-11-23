const assert = require('node:assert');

assert.strictEqual(import.meta.main, true);
assert.strictEqual(import.meta.filename, __filename);

const child = require('./nested/import_meta_main_child.cjs');
assert.strictEqual(child.main, false);
assert.strictEqual(child.readMain(), false);
assert.strictEqual(child.filename, require.resolve('./nested/import_meta_main_child.cjs'));

setTimeout(async () => {
  const late = (await import('./nested/import_meta_main_late.cjs')).default;
  assert.strictEqual(late.main, false);
  assert.strictEqual(late.filename, require.resolve('./nested/import_meta_main_late.cjs'));

  const esm = await import('./nested/import_meta_main_child.mjs');
  assert.strictEqual(esm.main, false);
  assert.strictEqual(import.meta.main, true);

  console.log('import.meta.main is true only for the CommonJS entry');
}, 5);
