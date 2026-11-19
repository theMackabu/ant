import assert from 'node:assert';

async function rejection(promise) {
  try { await promise; } catch (error) { return error; }
  assert.fail('expected a rejection');
}

for (let round = 0; round < 3; round++) {
  let error;
  error = await rejection(import.source('./fixtures/module-errors/plain.mjs'));
  assert.ok(error instanceof SyntaxError);
  assert.strictEqual(error.code, 'ERR_SOURCE_PHASE_NOT_DEFINED');
  assert.ok(error.message.startsWith("Source phase import object is not defined for module 'file://"));
  assert.ok(error.message.endsWith("/fixtures/module-errors/plain.mjs'"));

  error = await rejection(import.source('./fixtures/module-errors/nope.mjs'));
  assert.strictEqual(error.code, 'ERR_MODULE_NOT_FOUND');
}

assert.strictEqual(typeof new Function('return import.source("x")'), 'function');
assert.throws(() => new Function('return import.sourcex("x")'), SyntaxError);
assert.throws(() => new Function('return import.source'), SyntaxError);
assert.throws(() => new Function('return import.source(...a)'), SyntaxError);

console.log('ok');
