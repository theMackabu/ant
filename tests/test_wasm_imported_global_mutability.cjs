const assert = require('assert');

function u32(value) {
  const out = [];
  do {
    let byte = value & 0x7f;
    value >>>= 7;
    if (value) byte |= 0x80;
    out.push(byte);
  } while (value);
  return out;
}

function str(value) {
  const bytes = Array.from(Buffer.from(value));
  return [...u32(bytes.length), ...bytes];
}

function section(id, data) {
  return [id, ...u32(data.length), ...data];
}

function moduleBytes(sections) {
  return new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, ...sections.flat()]);
}

const wasmBytes = moduleBytes([
  section(1, [
    2,
    0x60, 0, 1, 0x7f,
    0x60, 1, 0x7f, 0,
  ]),
  section(2, [
    1,
    ...str('env'),
    ...str('g'),
    3,
    0x7f,
    1,
  ]),
  section(3, [2, 0, 1]),
  section(7, [
    2,
    ...str('read'), 0, 0,
    ...str('write'), 0, 1,
  ]),
  section(10, [
    2,
    4, 0, 0x23, 0, 0x0b,
    6, 0, 0x20, 0, 0x24, 0, 0x0b,
  ]),
]);

const global = new WebAssembly.Global({ value: 'i32', mutable: true }, 7);
global.value = 19;

const instance = new WebAssembly.Instance(new WebAssembly.Module(wasmBytes), {
  env: { g: global },
});

assert.strictEqual(instance.exports.read(), 19);

global.value = 23;
assert.strictEqual(instance.exports.read(), 23);

instance.exports.write(41);
assert.strictEqual(global.value, 41);
