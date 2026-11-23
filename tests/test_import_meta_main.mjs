import assert from 'node:assert';
import { createRequire } from 'node:module';
import * as child from './nested/import_meta_main_child.mjs';

const require = createRequire(import.meta.url);

assert.strictEqual(import.meta.main, true);
assert.strictEqual(child.main, false);
assert.strictEqual(child.readMain(), false);

const required = require('./nested/import_meta_main_child.cjs');
assert.strictEqual(required.main, false);
assert.strictEqual(required.readMain(), false);
assert.strictEqual(required.filename, require.resolve('./nested/import_meta_main_child.cjs'));

await new Promise(resolve => setTimeout(resolve, 5));

const late = (await import('./nested/import_meta_main_late.cjs')).default;
assert.strictEqual(late.main, false);
assert.strictEqual(late.filename, require.resolve('./nested/import_meta_main_late.cjs'));

const lateModule = await import('./nested/import_meta_main_child.mjs');
assert.strictEqual(lateModule.readMain(), false);
assert.strictEqual(import.meta.main, true);

console.log('import.meta.main is true only for the entry module');
