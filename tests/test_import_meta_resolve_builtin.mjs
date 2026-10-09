import assert from 'node:assert';

for (const [id, resolved] of [
  ['fs', 'node:fs'],
  ['node:fs', 'node:fs'],
  ['path', 'node:path'],
  ['node:http', 'node:http'],
  ['ant:ffi', 'ant:ffi'],
]) {
  assert.strictEqual(import.meta.resolve(id), resolved, id);
}

console.log('import-meta-resolve-builtin:ok');
