import {
  DEFAULT_MEMORY_LIMIT,
  INITIAL_MEMORY,
  MAX_MEMORY_LIMIT,
  RESPONSE_ERROR,
  RESPONSE_OK,
  RESPONSE_TIMEOUT,
  WASM_PAGE_SIZE
} from './abi.js';
import { AntDisposedError, AntTimeoutError, errorFrom } from './errors.js';
import { antImports } from './host.js';
import { allocateBytes, allocateString, checkedRange } from './memory.js';
import { getCompiledModule, getCrypto } from './module.js';
import { wasiImports } from './wasi.js';
import { decodeWire, encodeWire } from './wire.js';

export { AntDisposedError, AntTimeoutError };

const disposeSymbol = Symbol.dispose ?? Symbol.for('Symbol.dispose');

function maximumPages(memoryLimit) {
  if (!Number.isSafeInteger(memoryLimit) || memoryLimit < INITIAL_MEMORY || memoryLimit > MAX_MEMORY_LIMIT || memoryLimit % WASM_PAGE_SIZE !== 0) {
    throw new RangeError(`memoryLimit must be an integer aligned to ${WASM_PAGE_SIZE} bytes between ${INITIAL_MEMORY} and ${MAX_MEMORY_LIMIT} bytes`);
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
    if (options === null || typeof options !== 'object') throw new TypeError('Ant.create options must be an object');

    const timeout = normalizedTimeout(options.timeout);
    const globals = options.globals ?? {};
    if (globals === null || typeof globals !== 'object' || Array.isArray(globals)) throw new TypeError('globals must be an object');

    const memory = new WebAssembly.Memory({
      initial: INITIAL_MEMORY / WASM_PAGE_SIZE,
      maximum: maximumPages(options.memoryLimit ?? DEFAULT_MEMORY_LIMIT)
    });
    const crypto = await getCrypto();
    const module = await getCompiledModule();
    const hostFunctions = [];
    let exports;

    const instance = await WebAssembly.instantiate(module, {
      ant: antImports(memory, hostFunctions, () => exports),
      env: { memory },
      wasi_snapshot_preview1: wasiImports(memory, crypto)
    });
    exports = instance.exports;
    exports._initialize();

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
      pointer = allocateString(this.#exports, this.#memory, source);
      return this.#readResponse(this.#exports.ant_eval(this.#handle, pointer, source.length, this.#timeout));
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
    try {
      if (!pointer) throw new WebAssembly.RuntimeError('Ant WebAssembly memory limit exceeded');
      const length = this.#exports.ant_result_length(this.#handle);
      const response = new Uint8Array(this.#memory.buffer, checkedRange(this.#memory, pointer, length), length);
      const status = response[0];
      if (status === RESPONSE_TIMEOUT) throw new AntTimeoutError();
      if (length < 2 || (status !== RESPONSE_OK && status !== RESPONSE_ERROR)) {
        throw new WebAssembly.RuntimeError('Ant returned an invalid response');
      }
      const value = decodeWire(response.subarray(1));
      if (status === RESPONSE_ERROR) throw errorFrom(value);
      return value;
    } finally {
      this.#exports.ant_release_result(this.#handle);
    }
  }

  #setGlobal(name, value) {
    if (name.includes('\0')) throw new TypeError('global names cannot contain NUL characters');
    const namePointer = allocateString(this.#exports, this.#memory, name);
    let valuePointer = 0;
    let valueLength = 0;
    let functionId = -1;
    try {
      if (typeof value === 'function') functionId = this.#hostFunctions.push(value) - 1;
      else {
        const valueBytes = encodeWire(value);
        valueLength = valueBytes.length;
        valuePointer = allocateBytes(this.#exports, this.#memory, valueBytes);
      }
      this.#readResponse(this.#exports.ant_set_global(this.#handle, namePointer, name.length, valuePointer, valueLength, functionId));
    } catch (error) {
      if (functionId >= 0) this.#hostFunctions.pop();
      throw error;
    } finally {
      this.#exports.ant_free(namePointer);
      if (valuePointer) this.#exports.ant_free(valuePointer);
    }
  }
}
