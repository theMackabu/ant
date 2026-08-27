import {
  DEFAULT_MEMORY_LIMIT,
  INITIAL_MEMORY,
  MAX_MEMORY_LIMIT,
  RESPONSE_ERROR,
  RESPONSE_OK,
  RESPONSE_TIMEOUT,
  WASM_PAGE_SIZE,
  WIRE_ARRAY,
  WIRE_BIGINT,
  WIRE_FALSE,
  WIRE_MAX_BYTES as MAX_WIRE_BYTES,
  WIRE_MAX_CONTAINER_ENTRIES as MAX_WIRE_CONTAINER_ENTRIES,
  WIRE_MAX_DEPTH as MAX_WIRE_DEPTH,
  WIRE_NULL,
  WIRE_NUMBER,
  WIRE_OBJECT,
  WIRE_STRING,
  WIRE_TRUE,
  WIRE_UNDEFINED,
  WIRE_VERSION
} from './abi.js';
const WASI_ERRNO_BADF = 8;
const WASI_ERRNO_FAULT = 21;
const WASI_ERRNO_SPIPE = 70;
const disposeSymbol = Symbol.dispose ?? Symbol.for('Symbol.dispose');

let compiledModulePromise;

function encodeWtf8(value) {
  let byteLength = 0;
  for (let index = 0; index < value.length; index++) {
    const unit = value.charCodeAt(index);
    if (unit < 0x80) byteLength++;
    else if (unit < 0x800) byteLength += 2;
    else if (
      unit >= 0xd800 &&
      unit <= 0xdbff &&
      index + 1 < value.length &&
      value.charCodeAt(index + 1) >= 0xdc00 &&
      value.charCodeAt(index + 1) <= 0xdfff
    ) {
      byteLength += 4;
      index++;
    } else byteLength += 3;
  }

  const bytes = new Uint8Array(byteLength);
  let offset = 0;
  for (let index = 0; index < value.length; index++) {
    let codePoint = value.charCodeAt(index);
    if (codePoint >= 0xd800 && codePoint <= 0xdbff && index + 1 < value.length) {
      const low = value.charCodeAt(index + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + low - 0xdc00;
        index++;
      }
    }

    if (codePoint < 0x80) bytes[offset++] = codePoint;
    else if (codePoint < 0x800) {
      bytes[offset++] = 0xc0 | (codePoint >> 6);
      bytes[offset++] = 0x80 | (codePoint & 0x3f);
    } else if (codePoint < 0x10000) {
      bytes[offset++] = 0xe0 | (codePoint >> 12);
      bytes[offset++] = 0x80 | ((codePoint >> 6) & 0x3f);
      bytes[offset++] = 0x80 | (codePoint & 0x3f);
    } else {
      bytes[offset++] = 0xf0 | (codePoint >> 18);
      bytes[offset++] = 0x80 | ((codePoint >> 12) & 0x3f);
      bytes[offset++] = 0x80 | ((codePoint >> 6) & 0x3f);
      bytes[offset++] = 0x80 | (codePoint & 0x3f);
    }
  }
  return bytes;
}

function decodeWtf8(bytes) {
  const invalid = () => {
    throw new TypeError('Invalid @antjs.org/wasm string encoding');
  };
  const continuation = byte => (byte & 0xc0) === 0x80;
  let unitLength = 0;

  for (let offset = 0; offset < bytes.length; ) {
    const first = bytes[offset];
    if (first < 0x80) {
      offset++;
      unitLength++;
    } else if (first >= 0xc2 && first <= 0xdf) {
      if (offset + 1 >= bytes.length || !continuation(bytes[offset + 1])) invalid();
      offset += 2;
      unitLength++;
    } else if (first >= 0xe0 && first <= 0xef) {
      if (
        offset + 2 >= bytes.length ||
        !continuation(bytes[offset + 1]) ||
        !continuation(bytes[offset + 2]) ||
        (first === 0xe0 && bytes[offset + 1] < 0xa0)
      )
        invalid();
      offset += 3;
      unitLength++;
    } else if (first >= 0xf0 && first <= 0xf4) {
      if (
        offset + 3 >= bytes.length ||
        !continuation(bytes[offset + 1]) ||
        !continuation(bytes[offset + 2]) ||
        !continuation(bytes[offset + 3]) ||
        (first === 0xf0 && bytes[offset + 1] < 0x90) ||
        (first === 0xf4 && bytes[offset + 1] > 0x8f)
      )
        invalid();
      offset += 4;
      unitLength += 2;
    } else invalid();
  }

  const units = new Uint16Array(unitLength);
  let unitOffset = 0;
  for (let offset = 0; offset < bytes.length; ) {
    const first = bytes[offset++];
    let codePoint;
    if (first < 0x80) codePoint = first;
    else if (first < 0xe0) {
      codePoint = ((first & 0x1f) << 6) | (bytes[offset++] & 0x3f);
    } else if (first < 0xf0) {
      codePoint = ((first & 0x0f) << 12) | ((bytes[offset++] & 0x3f) << 6) | (bytes[offset++] & 0x3f);
    } else {
      codePoint = ((first & 0x07) << 18) | ((bytes[offset++] & 0x3f) << 12) | ((bytes[offset++] & 0x3f) << 6) | (bytes[offset++] & 0x3f);
    }

    if (codePoint < 0x10000) units[unitOffset++] = codePoint;
    else {
      codePoint -= 0x10000;
      units[unitOffset++] = 0xd800 | (codePoint >> 10);
      units[unitOffset++] = 0xdc00 | (codePoint & 0x3ff);
    }
  }

  let result = '';
  for (let offset = 0; offset < units.length; offset += 8192) result += String.fromCharCode(...units.subarray(offset, offset + 8192));
  return result;
}

class WireWriter {
  #bytes = new Uint8Array(256);
  #view = new DataView(this.#bytes.buffer);
  #length = 0;

  #reserve(extra) {
    const required = this.#length + extra;
    if (!Number.isSafeInteger(required) || required > MAX_WIRE_BYTES) {
      throw new RangeError('Transferred value is too large');
    }
    if (required <= this.#bytes.length) return;
    let capacity = this.#bytes.length;
    while (capacity < required) capacity = Math.min(capacity * 2, 0xffffffff);
    const bytes = new Uint8Array(capacity);
    bytes.set(this.#bytes.subarray(0, this.#length));
    this.#bytes = bytes;
    this.#view = new DataView(bytes.buffer);
  }

  u8(value) {
    this.#reserve(1);
    this.#bytes[this.#length++] = value;
  }

  u32(value) {
    this.#reserve(4);
    this.#view.setUint32(this.#length, value, true);
    this.#length += 4;
  }

  f64(value) {
    this.#reserve(8);
    this.#view.setFloat64(this.#length, value, true);
    this.#length += 8;
  }

  bytes(value) {
    this.#reserve(value.length);
    this.#bytes.set(value, this.#length);
    this.#length += value.length;
  }

  sizedBytes(value) {
    this.u32(value.length);
    this.bytes(value);
  }

  finish() {
    return this.#bytes.subarray(0, this.#length);
  }
}

class WireReader {
  #bytes;
  #view;
  #offset = 0;

  constructor(bytes) {
    this.#bytes = bytes;
    this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  #require(length) {
    if (length > this.#bytes.length - this.#offset) {
      throw new TypeError('Invalid @antjs.org/wasm value');
    }
  }

  #take(length) {
    this.#require(length);
    const bytes = this.#bytes.subarray(this.#offset, this.#offset + length);
    this.#offset += length;
    return bytes;
  }

  u8() {
    this.#require(1);
    return this.#bytes[this.#offset++];
  }

  u32() {
    this.#require(4);
    const value = this.#view.getUint32(this.#offset, true);
    this.#offset += 4;
    return value;
  }

  f64() {
    this.#require(8);
    const value = this.#view.getFloat64(this.#offset, true);
    this.#offset += 8;
    return value;
  }

  sizedBytes() {
    return this.#take(this.u32());
  }

  get done() {
    return this.#offset === this.#bytes.length;
  }
}

function encodeWire(value, responseStatus) {
  const writer = new WireWriter();
  const active = new Set();
  if (responseStatus !== undefined) writer.u8(responseStatus);
  writer.u8(WIRE_VERSION);

  const visit = (item, depth) => {
    if (depth > MAX_WIRE_DEPTH) throw new TypeError('Transferred value is too deeply nested');
    if (item === null) {
      writer.u8(WIRE_NULL);
      return;
    }

    switch (typeof item) {
      case 'undefined':
        writer.u8(WIRE_UNDEFINED);
        return;
      case 'boolean':
        writer.u8(item ? WIRE_TRUE : WIRE_FALSE);
        return;
      case 'number':
        writer.u8(WIRE_NUMBER);
        writer.f64(item);
        return;
      case 'string':
        writer.u8(WIRE_STRING);
        writer.sizedBytes(encodeWtf8(item));
        return;
      case 'bigint':
        writer.u8(WIRE_BIGINT);
        writer.sizedBytes(encodeWtf8(String(item)));
        return;
      case 'object': {
        if (active.has(item)) throw new TypeError('Cannot transfer cyclic values');
        active.add(item);
        if (Array.isArray(item)) {
          if (item.length > MAX_WIRE_CONTAINER_ENTRIES) {
            throw new RangeError('Transferred array has too many entries');
          }
          writer.u8(WIRE_ARRAY);
          writer.u32(item.length);
          for (const value of item) visit(value, depth + 1);
        } else {
          const keys = Object.keys(item);
          if (keys.length > MAX_WIRE_CONTAINER_ENTRIES) {
            throw new RangeError('Transferred object has too many entries');
          }
          writer.u8(WIRE_OBJECT);
          writer.u32(keys.length);
          for (const key of keys) {
            writer.sizedBytes(encodeWtf8(key));
            visit(item[key], depth + 1);
          }
        }
        active.delete(item);
        return;
      }
      default:
        throw new TypeError(`Cannot transfer ${typeof item} values`);
    }
  };

  visit(value, 0);
  return writer.finish();
}

function decodeWire(bytes) {
  const reader = new WireReader(bytes);
  if (reader.u8() !== WIRE_VERSION) {
    throw new TypeError('Invalid @antjs.org/wasm value version');
  }

  const visit = depth => {
    if (depth > MAX_WIRE_DEPTH) throw new TypeError('Transferred value is too deeply nested');
    switch (reader.u8()) {
      case WIRE_UNDEFINED:
        return undefined;
      case WIRE_NULL:
        return null;
      case WIRE_FALSE:
        return false;
      case WIRE_TRUE:
        return true;
      case WIRE_NUMBER:
        return reader.f64();
      case WIRE_STRING:
        return decodeWtf8(reader.sizedBytes());
      case WIRE_BIGINT:
        return BigInt(decodeWtf8(reader.sizedBytes()));
      case WIRE_ARRAY: {
        const length = reader.u32();
        if (length > MAX_WIRE_CONTAINER_ENTRIES) {
          throw new RangeError('Transferred array has too many entries');
        }
        return Array.from({ length }, () => visit(depth + 1));
      }
      case WIRE_OBJECT: {
        const length = reader.u32();
        if (length > MAX_WIRE_CONTAINER_ENTRIES) {
          throw new RangeError('Transferred object has too many entries');
        }
        const result = {};
        for (let index = 0; index < length; index++) {
          const key = decodeWtf8(reader.sizedBytes());
          Object.defineProperty(result, key, {
            configurable: true,
            enumerable: true,
            writable: true,
            value: visit(depth + 1)
          });
        }
        return result;
      }
      default:
        throw new TypeError('Invalid @antjs.org/wasm value tag');
    }
  };

  const value = visit(0);
  if (!reader.done) throw new TypeError('Invalid trailing @antjs.org/wasm data');
  return value;
}

function transferableError(error) {
  let name = 'Error';
  let message;
  let stack;
  if (error !== null && (typeof error === 'object' || typeof error === 'function')) {
    try {
      if (typeof error.name === 'string' && error.name) name = error.name;
    } catch {}
    try {
      if (typeof error.message === 'string' && error.message) message = error.message;
    } catch {}
    try {
      if (typeof error.stack === 'string') stack = error.stack;
    } catch {}
  }
  if (!message) {
    try {
      message = String(error);
    } catch {
      message = 'Host function failed';
    }
  }
  return stack === undefined ? { name, message } : { name, message, stack };
}

function errorFrom(value) {
  const name = typeof value?.name === 'string' ? value.name : 'Error';
  const message = typeof value?.message === 'string' ? value.message : 'Ant evaluation failed';

  let error;
  if (name === 'TypeError') error = new TypeError(message);
  else if (name === 'SyntaxError') error = new SyntaxError(message);
  else if (name === 'ReferenceError') error = new ReferenceError(message);
  else if (name === 'RangeError') error = new RangeError(message);
  else error = new Error(message);

  error.name = name;
  if (typeof value?.stack === 'string') error.stack = value.stack;
  return error;
}

function maximumPages(memoryLimit) {
  if (!Number.isSafeInteger(memoryLimit) || memoryLimit < INITIAL_MEMORY || memoryLimit > MAX_MEMORY_LIMIT || memoryLimit % WASM_PAGE_SIZE !== 0) {
    throw new RangeError(`memoryLimit must be an integer aligned to 65536 bytes between ${INITIAL_MEMORY} and ${MAX_MEMORY_LIMIT} bytes`);
  }
  return memoryLimit / WASM_PAGE_SIZE;
}

function normalizedTimeout(timeout) {
  if (timeout === undefined) return 0;
  if (!Number.isSafeInteger(timeout) || timeout < 0 || timeout > 0xffffffff) {
    throw new RangeError('timeout must be an integer from 0 through 4294967295 milliseconds');
  }
  return timeout;
}

function checkedRange(memory, pointer, length) {
  pointer >>>= 0;
  length >>>= 0;
  if (pointer > memory.buffer.byteLength || length > memory.buffer.byteLength - pointer) {
    throw new WebAssembly.RuntimeError('Ant WebAssembly memory access is out of bounds');
  }
  return pointer;
}

function writeU32(memory, pointer, value) {
  pointer = checkedRange(memory, pointer, 4);
  new DataView(memory.buffer).setUint32(pointer, value, true);
}

function fillRandom(crypto, memory, pointer, length) {
  try {
    pointer = checkedRange(memory, pointer, length);
    for (let offset = 0; offset < length; offset += 65_536) {
      const size = Math.min(65_536, length - offset);
      crypto.getRandomValues(new Uint8Array(memory.buffer, pointer + offset, size));
    }
    return 0;
  } catch {
    return WASI_ERRNO_FAULT;
  }
}

function wasiImports(memory, crypto) {
  return {
    environ_get: () => 0,
    environ_sizes_get: (count, size) => {
      writeU32(memory, count, 0);
      writeU32(memory, size, 0);
      return 0;
    },
    fd_close: () => WASI_ERRNO_BADF,
    fd_prestat_dir_name: () => WASI_ERRNO_BADF,
    fd_prestat_get: () => WASI_ERRNO_BADF,
    fd_seek: () => WASI_ERRNO_SPIPE,
    fd_write: (_fd, iovecs, count, written) => {
      try {
        let length = 0;
        for (let index = 0; index < count; index++) {
          const entry = checkedRange(memory, iovecs + index * 8, 8);
          const view = new DataView(memory.buffer);
          const pointer = view.getUint32(entry, true);
          const partLength = view.getUint32(entry + 4, true);
          checkedRange(memory, pointer, partLength);
          length = (length + partLength) >>> 0;
        }
        writeU32(memory, written, length);
        return 0;
      } catch {
        return WASI_ERRNO_FAULT;
      }
    },
    proc_exit: status => {
      throw new WebAssembly.RuntimeError(`Ant WASI process exited with status ${status}`);
    },
    random_get: (pointer, length) => fillRandom(crypto, memory, pointer, length)
  };
}

async function compileModule() {
  const url = new URL('./ant.wasm', import.meta.url);
  if (url.protocol === 'file:') {
    const { readFile } = await import('node:fs/promises');
    return WebAssembly.compile(await readFile(url));
  }

  const response = await fetch(url);
  if (!response.ok) throw new Error(`Unable to load ${url}: HTTP ${response.status}`);
  if (typeof WebAssembly.compileStreaming === 'function') {
    try {
      return await WebAssembly.compileStreaming(response.clone());
    } catch {
      // Servers without application/wasm still work through the byte fallback.
    }
  }
  return WebAssembly.compile(await response.arrayBuffer());
}

function getCompiledModule() {
  compiledModulePromise ??= compileModule().catch(error => {
    compiledModulePromise = undefined;
    throw error;
  });
  return compiledModulePromise;
}

async function getCrypto() {
  if (globalThis.crypto?.getRandomValues) return globalThis.crypto;
  if (typeof process !== 'undefined' && process.versions?.node) {
    return (await import('node:crypto')).webcrypto;
  }
  throw new WebAssembly.RuntimeError('Web Crypto is required by @antjs.org/wasm');
}

function allocateBytes(exports, memory, bytes) {
  const pointer = exports.ant_alloc(Math.max(1, bytes.length));
  if (!pointer) throw new WebAssembly.RuntimeError('Ant WebAssembly memory limit exceeded');
  checkedRange(memory, pointer, bytes.length);
  new Uint8Array(memory.buffer, pointer, bytes.length).set(bytes);
  return pointer;
}

export class AntTimeoutError extends Error {
  constructor(message = 'Execution timed out') {
    super(message);
    this.name = 'AntTimeoutError';
  }
}

export class AntDisposedError extends Error {
  constructor() {
    super('This Ant instance has been disposed');
    this.name = 'AntDisposedError';
  }
}

export class Ant {
  #exports;
  #memory;
  #handle;
  #timeout;
  #hostFunctions;
  #evaluating = false;

  constructor(token, exports, memory, handle, timeout, hostFunctions) {
    if (token !== Ant) throw new TypeError('Use Ant.create() to create an instance');
    this.#exports = exports;
    this.#memory = memory;
    this.#handle = handle;
    this.#timeout = timeout;
    this.#hostFunctions = hostFunctions;
  }

  static async create(options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TypeError('Ant.create options must be an object');
    }

    const memoryLimit = options.memoryLimit ?? DEFAULT_MEMORY_LIMIT;
    const timeout = normalizedTimeout(options.timeout);
    const globals = options.globals ?? {};
    if (globals === null || typeof globals !== 'object' || Array.isArray(globals)) {
      throw new TypeError('globals must be an object');
    }

    const memory = new WebAssembly.Memory({
      initial: INITIAL_MEMORY / WASM_PAGE_SIZE,
      maximum: maximumPages(memoryLimit)
    });
    const crypto = await getCrypto();
    const module = await getCompiledModule();
    const hostFunctions = [];
    let exports;
    const antImports = {
      host_call(functionId, argumentsPointer, argumentsLength, responseLengthPointer) {
        let response;
        try {
          argumentsPointer = checkedRange(memory, argumentsPointer, argumentsLength);
          const argumentsBytes = new Uint8Array(memory.buffer, argumentsPointer, argumentsLength);
          const fn = hostFunctions[functionId];
          if (typeof fn !== 'function') throw new TypeError('Unknown host function');
          const args = decodeWire(argumentsBytes);
          const value = fn(...args);
          if (value !== null && (typeof value === 'object' || typeof value === 'function') && typeof value.then === 'function') {
            throw new TypeError('Host functions must return synchronously');
          }
          response = encodeWire(value, RESPONSE_OK);
        } catch (error) {
          try {
            response = encodeWire(transferableError(error), RESPONSE_ERROR);
          } catch {
            try {
              response = encodeWire(
                {
                  name: 'Error',
                  message: 'Host function failed'
                },
                RESPONSE_ERROR
              );
            } catch {
              writeU32(memory, responseLengthPointer, 0);
              return 0;
            }
          }
        }

        const pointer = exports.ant_alloc(response.length);
        if (!pointer) {
          writeU32(memory, responseLengthPointer, 0);
          return 0;
        }
        checkedRange(memory, pointer, response.length);
        new Uint8Array(memory.buffer, pointer, response.length).set(response);
        writeU32(memory, responseLengthPointer, response.length);
        return pointer;
      },
      now_ms: () => globalThis.performance?.now?.() ?? Date.now(),
      random_fill: (pointer, length) => fillRandom(crypto, memory, pointer, length)
    };

    const instance = await WebAssembly.instantiate(module, {
      ant: antImports,
      env: { memory },
      wasi_snapshot_preview1: wasiImports(memory, crypto)
    });
    exports = instance.exports;
    exports._initialize?.();

    const handle = exports.ant_create();
    if (!handle) throw new WebAssembly.RuntimeError('Unable to initialize Ant');

    const ant = new Ant(Ant, exports, memory, handle, timeout, hostFunctions);
    try {
      for (const [name, value] of Object.entries(globals)) ant.#setGlobal(name, value);
      return ant;
    } catch (error) {
      ant.dispose();
      throw error;
    }
  }

  async eval(source) {
    this.#assertUsable();
    if (typeof source !== 'string') throw new TypeError('source must be a string');
    if (source.includes('\0')) throw new TypeError('source cannot contain NUL characters');
    if (this.#evaluating) throw new Error('Ant.eval cannot be called recursively');

    this.#evaluating = true;
    let pointer = 0;
    try {
      const bytes = encodeWtf8(source);
      pointer = allocateBytes(this.#exports, this.#memory, bytes);
      const resultPointer = this.#exports.ant_eval(this.#handle, pointer, bytes.length, this.#timeout);
      return this.#readResponse(resultPointer);
    } finally {
      if (pointer) this.#exports.ant_free(pointer);
      this.#evaluating = false;
    }
  }

  dispose() {
    if (!this.#handle) return;
    if (this.#evaluating) throw new Error('Ant cannot be disposed during evaluation');
    this.#exports.ant_destroy(this.#handle);
    this.#hostFunctions.length = 0;
    this.#handle = 0;
  }

  [disposeSymbol]() {
    this.dispose();
  }

  #assertUsable() {
    if (!this.#handle) throw new AntDisposedError();
  }

  #readResponse(pointer) {
    if (!pointer) {
      this.#exports.ant_release_result(this.#handle);
      throw new WebAssembly.RuntimeError('Ant WebAssembly memory limit exceeded');
    }
    try {
      const length = this.#exports.ant_result_length(this.#handle);
      pointer = checkedRange(this.#memory, pointer, length);
      if (length < 1) throw new WebAssembly.RuntimeError('Ant returned an invalid response');
      const response = new Uint8Array(this.#memory.buffer, pointer, length);
      const status = response[0];
      if (status === RESPONSE_TIMEOUT) throw new AntTimeoutError();
      if (length < 2) throw new WebAssembly.RuntimeError('Ant returned an invalid response');
      const value = decodeWire(response.subarray(1));
      if (status === RESPONSE_ERROR) throw errorFrom(value);
      if (status !== RESPONSE_OK) {
        throw new WebAssembly.RuntimeError('Ant returned an invalid response');
      }
      return value;
    } finally {
      this.#exports.ant_release_result(this.#handle);
    }
  }

  #setGlobal(name, value) {
    if (name.includes('\0')) throw new TypeError('global names cannot contain NUL characters');
    const nameBytes = encodeWtf8(name);
    const namePointer = allocateBytes(this.#exports, this.#memory, nameBytes);
    let valuePointer = 0;
    let valueLength = 0;
    let functionId = -1;
    try {
      if (typeof value === 'function') {
        functionId = this.#hostFunctions.push(value) - 1;
      } else {
        const valueBytes = encodeWire(value);
        valueLength = valueBytes.length;
        valuePointer = allocateBytes(this.#exports, this.#memory, valueBytes);
      }
      const response = this.#exports.ant_set_global(this.#handle, namePointer, nameBytes.length, valuePointer, valueLength, functionId);
      this.#readResponse(response);
    } catch (error) {
      if (functionId >= 0) this.#hostFunctions.pop();
      throw error;
    } finally {
      this.#exports.ant_free(namePointer);
      if (valuePointer) this.#exports.ant_free(valuePointer);
    }
  }
}
