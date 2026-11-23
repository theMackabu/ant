const assert = require('node:assert');
const { alloc, callback, dlopen, suffix, FFIType } = require('ant:ffi');

let libcName = `libc.${suffix}`;
if (process.platform === 'darwin') libcName = 'libSystem.dylib';
if (process.platform === 'linux') libcName = 'libc.so.6';
if (process.platform === 'win32') libcName = 'msvcrt.dll';

let libc = null;
let bsearch = null;
try {
  libc = dlopen(libcName);
  bsearch = libc.define('bsearch', {
    args: [FFIType.pointer, FFIType.pointer, FFIType.uint64, FFIType.uint64, FFIType.pointer],
    returns: FFIType.pointer,
  });
} catch {}

if (!bsearch) {
  console.log(`FFI error boundaries skipped: no bsearch in ${libcName}`);
} else {
  const count = 64;
  const size = 4;
  const key = alloc(size);
  const values = alloc(count * size);
  key.write(FFIType.int, count);
  for (let i = 0; i < count; i++) values.offset(i * size).write(FFIType.int, i);

  function search(returns, first, fail) {
    let calls = 0;
    let failedAt = 0;
    const compare = callback({ args: [FFIType.pointer, FFIType.pointer], returns }, (_, element) => {
      if (++calls === 1) return first;
      failedAt ||= element.address();
      return fail();
    });
    try {
      const found = bsearch(key, values, count, size, compare);
      return { calls, failedAt, found: found === null ? 0 : found.address() };
    } finally {
      compare.close();
    }
  }

  const missing = search(FFIType.int, 1, () => 1);
  assert.ok(missing.calls > 2, 'bsearch must keep probing while the comparator returns non-zero');
  assert.strictEqual(missing.found, 0);

  for (const [returns, first, incompatible] of [
    [FFIType.int, 1, () => Symbol()],
    [FFIType.pointer, values, () => ({})],
  ]) {
    for (const fail of [
      () => { throw undefined; },
      () => { throw null; },
      () => { throw new Error('FFI callback'); },
      incompatible,
    ]) {
      const result = search(returns, first, fail);
      assert.strictEqual(result.calls, 2, 'a failed callback must return a zero value');
      assert.notStrictEqual(result.failedAt, 0);
      assert.strictEqual(result.found, result.failedAt, 'a failed callback must return a zero value');
    }
  }

  key.free();
  values.free();
  libc.close();
  queueMicrotask(() => console.log('FFI error boundaries ok'));
}
