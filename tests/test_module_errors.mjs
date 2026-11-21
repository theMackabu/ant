import assert from 'node:assert';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const self = fileURLToPath(import.meta.url);
const require = createRequire(import.meta.url);

async function rejection(promise) {
  try { await promise; } catch (error) { return error; }
  assert.fail('expected a rejection');
}

function thrown(fn) {
  try { fn(); } catch (error) { return error; }
  assert.fail('expected a throw');
}

for (let round = 0; round < 3; round++) {
  let error = await rejection(import('./fixtures/module-errors/nope.mjs'));
  assert.ok(error instanceof Error);
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
  assert.strictEqual(error.message, `Cannot find module '${path.join(here, 'fixtures/module-errors/nope.mjs')}' imported from ${self}`);

  error = await rejection(import('../tests/./fixtures/../nope2.mjs'));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
  assert.strictEqual(error.message, `Cannot find module '${path.join(here, 'nope2.mjs')}' imported from ${self}`);

  error = await rejection(import('/nonexistent-ant-test/zz.mjs'));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
  assert.strictEqual(error.message, `Cannot find module '/nonexistent-ant-test/zz.mjs' imported from ${self}`);

  error = await rejection(import('no-such-pkg-ant-test'));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
  assert.strictEqual(error.message, `Cannot find package 'no-such-pkg-ant-test' imported from ${self}`);

  error = await rejection(import('no-such-pkg-ant-test/sub/file.js'));
  assert.strictEqual(error.message, `Cannot find package 'no-such-pkg-ant-test' imported from ${self}`);

  error = await rejection(import('@no-scope-ant/pkg/sub.js'));
  assert.strictEqual(error.message, `Cannot find package '@no-scope-ant/pkg' imported from ${self}`);

  error = await rejection(import(5));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
  assert.strictEqual(error.message, `Cannot find package '5' imported from ${self}`);

  error = await rejection(import({ toString() { return './fixtures/module-errors/nope3.mjs'; } }));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');

  error = await rejection(import('node:no-such-builtin'));
  assert.strictEqual(error.code, 'ERR_UNKNOWN_BUILTIN_MODULE');
  assert.strictEqual(error.message, 'No such built-in module: node:no-such-builtin');

  error = thrown(() => require('./fixtures/module-errors/nope.js'));
  assert.ok(error instanceof Error);
  assert.strictEqual(error.code, 'MODULE_NOT_FOUND');
  assert.strictEqual(error.message.split('\n')[0], "Cannot find module './fixtures/module-errors/nope.js'");

  error = thrown(() => require('no-such-pkg-ant-test'));
  assert.strictEqual(error.code, 'MODULE_NOT_FOUND');
  assert.strictEqual(error.message.split('\n')[0], "Cannot find module 'no-such-pkg-ant-test'");

  error = thrown(() => require('node:no-such-builtin'));
  assert.strictEqual(error.code, 'ERR_UNKNOWN_BUILTIN_MODULE');

  error = thrown(() => require('./fixtures/module-errors/tla.mjs'));
  assert.strictEqual(error.code, 'ERR_REQUIRE_ASYNC_MODULE');
  assert.ok(error.message.startsWith('require() cannot be used on an ESM graph with top-level await. Use import() instead.'));

  error = thrown(() => require('./fixtures/module-errors/tla-user.mjs'));
  assert.strictEqual(error.code, 'ERR_REQUIRE_ASYNC_MODULE');

  assert.strictEqual(require('./fixtures/module-errors/plain.mjs').plain, 3);
  assert.strictEqual((await import('./fixtures/module-errors/tla-user.mjs')).y, 2);

}

console.log('ok');
