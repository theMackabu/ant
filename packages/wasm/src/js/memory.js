import { writeUtf16 } from './utf16.js';

export function checkedRange(memory, pointer, length) {
  pointer >>>= 0;
  length >>>= 0;
  if (pointer > memory.buffer.byteLength || length > memory.buffer.byteLength - pointer) {
    throw new WebAssembly.RuntimeError('Ant WebAssembly memory access is out of bounds');
  }
  return pointer;
}

export function writeU32(memory, pointer, value) {
  new DataView(memory.buffer).setUint32(checkedRange(memory, pointer, 4), value, true);
}

export function copyIn(exports, memory, bytes) {
  const pointer = exports.ant_alloc(Math.max(1, bytes.length));
  if (!pointer) return 0;
  new Uint8Array(memory.buffer, checkedRange(memory, pointer, bytes.length), bytes.length).set(bytes);
  return pointer;
}

export function allocateBytes(exports, memory, bytes) {
  const pointer = copyIn(exports, memory, bytes);
  if (!pointer) throw new WebAssembly.RuntimeError('Ant WebAssembly memory limit exceeded');
  return pointer;
}

export function allocateString(exports, memory, value) {
  const length = value.length * 2;
  const pointer = exports.ant_alloc(Math.max(1, length));
  if (!pointer) throw new WebAssembly.RuntimeError('Ant WebAssembly memory limit exceeded');
  writeUtf16(new DataView(memory.buffer), checkedRange(memory, pointer, length), value);
  return pointer;
}
