import { buildWasm } from './build';

const i32 = 0x7f;
const local_get = i => [0x20, i];
const i32_add = [0x6a];

const wasmBytes = buildWasm({
  types: [[[i32, i32], [i32]]],
  funcs: [{ body: [...local_get(0), ...local_get(1), ...i32_add] }],
  exports: [['add', 0x00, 0]]
});

console.log(`compiled to ${wasmBytes.length} bytes\n`);
const { instance } = await WebAssembly.instantiate(wasmBytes);

console.log('add(2, 3) =', instance.exports.add(2, 3));
console.log('add(100, 200) =', instance.exports.add(100, 200));
