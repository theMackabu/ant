import assert from 'node:assert';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as child from './nested/import_meta_aliases_child.mjs';

function checkMeta(meta, expectedPath) {
  assert.strictEqual(meta.dir, path.dirname(expectedPath));
  assert.strictEqual(meta.dirname, meta.dir);
  assert.strictEqual(meta.path, expectedPath);
  assert.strictEqual(meta.filename, expectedPath);
  assert.strictEqual(meta.file, path.basename(expectedPath));
  assert.strictEqual(meta.env, process.env);
}

async function main() {
  const ownPath = fileURLToPath(import.meta.url);
  const childPath = path.join(path.dirname(ownPath), 'nested', 'import_meta_aliases_child.mjs');
  checkMeta(import.meta, ownPath);
  checkMeta(child.meta, childPath);
  checkMeta(child.readMeta(), childPath);
  checkMeta(await child.readAsyncMeta(), childPath);

  const dynamic = await import('./nested/import_meta_aliases_child.mjs');
  checkMeta(await dynamic.readAsyncMeta(), childPath);
  checkMeta(import.meta, ownPath);

  const env = process.env;
  const descriptor = Object.getOwnPropertyDescriptor(import.meta, 'env');
  assert.strictEqual(descriptor.value, env);
  assert.strictEqual(descriptor.get, undefined);
  const originalProcess = globalThis.process;
  try {
    globalThis.process = { env: {} };
    assert.strictEqual(import.meta.env, env);
    assert.strictEqual(child.meta.env, env);
  } finally {
    globalThis.process = originalProcess;
  }

  const key = 'ANT_TEST_IMPORT_META_ENV';
  const previous = process.env[key];
  try {
    process.env[key] = 'from-process';
    assert.strictEqual(import.meta.env[key], 'from-process');
    child.meta.env[key] = 'from-child';
    assert.strictEqual(process.env[key], 'from-child');
    assert.strictEqual(import.meta.env[key], 'from-child');
  } finally {
    if (previous === undefined) delete process.env[key];
    else process.env[key] = previous;
  }

  console.log('import.meta aliases passed');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
