const assert = require('node:assert');
const { callback, FFIType } = require('ant:ffi');

const invoke = Ant.unsafe.c({ entry: 'invoke', args: ['uint64'], returns: 'int' })`
  #include <stdint.h>
  int invoke(uint64_t address) {
    void *(*fn)(void) = (void *(*)(void))(uintptr_t)address;
    return fn() == 0;
  }
`;

for (const fn of [
  () => { throw undefined; },
  () => { throw null; },
  () => { throw new Error('FFI callback'); },
  () => ({}),
]) {
  const cb = callback({ args: [], returns: FFIType.pointer }, fn);
  try {
    assert.strictEqual(invoke(BigInt(cb.address())), 1, 'failed callbacks must return a zero pointer');
  } finally {
    cb.close();
  }
}
queueMicrotask(() => console.log('FFI error boundaries ok'));
