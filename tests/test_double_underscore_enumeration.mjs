function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const plain = { __x: 1, visible: 2 };
assert(
  JSON.stringify(Object.keys(plain)) === JSON.stringify(['__x', 'visible']),
  'Object.keys() should retain double-underscore own properties'
);
assert(
  JSON.stringify(Object.entries(plain)) === JSON.stringify([['__x', 1], ['visible', 2]]),
  'Object.entries() should retain double-underscore own properties'
);
assert(
  JSON.stringify(Reflect.ownKeys(plain)) === JSON.stringify(['__x', 'visible']),
  'Reflect.ownKeys() should retain double-underscore own properties'
);

const forInKeys = [];
for (const key in plain) forInKeys.push(key);
assert(
  JSON.stringify(forInKeys) === JSON.stringify(['__x', 'visible']),
  'for...in should retain double-underscore own properties'
);

const ctorBytes = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 5, 1, 96, 0, 1, 127,
  3, 2, 1, 0,
  7, 21, 1, 17, 95, 95, 119, 97, 115, 109, 95, 99, 97, 108, 108, 95, 99, 116, 111, 114, 115, 0, 0,
  10, 6, 1, 4, 0, 65, 1, 11,
]);

const module = new WebAssembly.Module(ctorBytes);
const instance = new WebAssembly.Instance(module);
assert(
  typeof instance.exports.__wasm_call_ctors === 'function',
  'wasm export named __wasm_call_ctors should be callable'
);
assert(
  JSON.stringify(Object.keys(instance.exports)) === JSON.stringify(['__wasm_call_ctors']),
  'Object.keys(instance.exports) should retain __wasm_call_ctors'
);

const copiedExports = Object.fromEntries(Object.entries(instance.exports));
assert(
  typeof copiedExports.__wasm_call_ctors === 'function',
  'Object.entries(instance.exports) should preserve __wasm_call_ctors for Asyncify-style wrappers'
);
assert(copiedExports.__wasm_call_ctors() === 1, 'copied wasm export should still execute');

console.log('double-underscore-enumeration:ok');
