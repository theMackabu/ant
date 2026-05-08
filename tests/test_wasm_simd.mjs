function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const simdModule = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 7, 7, 1, 3, 114, 117, 110, 0, 0, 10, 23, 1, 21, 0, 0xfd, 0x0c, 1, 2, 3, 4, 5, 6, 7, 8,
  9, 10, 11, 12, 13, 14, 15, 16, 0x1a, 0x0b
]);

assert(WebAssembly.validate(simdModule), 'SIMD-prefixed wasm should validate');

const instance = new WebAssembly.Instance(new WebAssembly.Module(simdModule));
assert(typeof instance.exports.run === 'function', 'SIMD module should instantiate');
instance.exports.run();

console.log('wasm:simd:ok');
