import { checkedRange, writeU32 } from './memory.js';

const ERRNO_BADF = 8;
const ERRNO_FAULT = 21;
const ERRNO_SPIPE = 70;
const RIGHTS_FD_WRITE = 1n << 6n;

function faultOnThrow(operation) {
  try {
    operation();
    return 0;
  } catch {
    return ERRNO_FAULT;
  }
}

export function wasiImports(memory, crypto) {
  const isOutput = fd => fd === 1 || fd === 2;
  return {
    clock_time_get: (id, precision, pointer) => {
      const now = globalThis.performance?.now?.();
      const ms = id === 0
        ? (now === undefined ? Date.now() : (globalThis.performance.timeOrigin ?? Date.now() - now) + now)
        : (now ?? Date.now());
      return faultOnThrow(() => {
        new DataView(memory.buffer).setBigUint64(checkedRange(memory, pointer, 8), BigInt(Math.round(ms * 1e6)), true);
      });
    },
    environ_get: () => 0,
    environ_sizes_get: (count, size) => {
      writeU32(memory, count, 0);
      writeU32(memory, size, 0);
      return 0;
    },
    fd_close: () => ERRNO_BADF,
    fd_fdstat_get: (fd, pointer) => {
      if (!isOutput(fd)) return ERRNO_BADF;
      return faultOnThrow(() => {
        pointer = checkedRange(memory, pointer, 24);
        new Uint8Array(memory.buffer, pointer, 24).fill(0);
        new DataView(memory.buffer).setBigUint64(pointer + 8, RIGHTS_FD_WRITE, true);
      });
    },
    fd_prestat_dir_name: () => ERRNO_BADF,
    fd_prestat_get: () => ERRNO_BADF,
    fd_seek: () => ERRNO_SPIPE,
    fd_write: (fd, iovecs, count, written) => {
      if (!isOutput(fd)) return ERRNO_BADF;
      return faultOnThrow(() => {
        let length = 0;
        for (let index = 0; index < count; index++) {
          const entry = checkedRange(memory, iovecs + index * 8, 8);
          const view = new DataView(memory.buffer);
          const partLength = view.getUint32(entry + 4, true);
          checkedRange(memory, view.getUint32(entry, true), partLength);
          length = (length + partLength) >>> 0;
        }
        writeU32(memory, written, length);
      });
    },
    proc_exit: status => {
      throw new WebAssembly.RuntimeError(`Ant WASI process exited with status ${status}`);
    },
    random_get: (pointer, length) =>
      faultOnThrow(() => {
        pointer = checkedRange(memory, pointer, length);
        for (let offset = 0; offset < length; offset += 65_536) {
          crypto.getRandomValues(new Uint8Array(memory.buffer, pointer + offset, Math.min(65_536, length - offset)));
        }
      })
  };
}
