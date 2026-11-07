import {
  WIRE_ARRAY,
  WIRE_BIGINT,
  WIRE_FALSE,
  WIRE_MAX_BYTES,
  WIRE_MAX_CONTAINER_ENTRIES,
  WIRE_MAX_DEPTH,
  WIRE_NULL,
  WIRE_NUMBER,
  WIRE_OBJECT,
  WIRE_STRING,
  WIRE_TRUE,
  WIRE_UNDEFINED,
  WIRE_VERSION
} from './abi.js';
import { readUtf16, writeUtf16 } from './utf16.js';

class WireWriter {
  #bytes = new Uint8Array(256);
  #view = new DataView(this.#bytes.buffer);
  #length = 0;

  #reserve(extra) {
    const required = this.#length + extra;
    if (required > WIRE_MAX_BYTES) throw new RangeError('Transferred value is too large');
    if (required <= this.#bytes.length) return;
    let capacity = this.#bytes.length;
    while (capacity < required) capacity *= 2;
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

  string(value) {
    this.u32(value.length);
    this.#reserve(value.length * 2);
    writeUtf16(this.#view, this.#length, value);
    this.#length += value.length * 2;
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

  #advance(length) {
    if (length > this.#bytes.length - this.#offset) throw new TypeError('Invalid @antjs.org/wasm value');
    const offset = this.#offset;
    this.#offset += length;
    return offset;
  }

  u8() {
    return this.#bytes[this.#advance(1)];
  }

  u32() {
    return this.#view.getUint32(this.#advance(4), true);
  }

  f64() {
    return this.#view.getFloat64(this.#advance(8), true);
  }

  string() {
    const count = this.u32();
    return readUtf16(this.#view, this.#advance(count * 2), count);
  }

  get done() {
    return this.#offset === this.#bytes.length;
  }
}

function checkedEntries(length, kind) {
  if (length > WIRE_MAX_CONTAINER_ENTRIES) throw new RangeError(`Transferred ${kind} has too many entries`);
  return length;
}

export function encodeWire(value, responseStatus) {
  const writer = new WireWriter();
  const active = new Set();
  if (responseStatus !== undefined) writer.u8(responseStatus);
  writer.u8(WIRE_VERSION);

  const visit = (item, depth) => {
    if (depth > WIRE_MAX_DEPTH) throw new TypeError('Transferred value is too deeply nested');
    if (item === null) return writer.u8(WIRE_NULL);

    switch (typeof item) {
      case 'undefined':
        return writer.u8(WIRE_UNDEFINED);
      case 'boolean':
        return writer.u8(item ? WIRE_TRUE : WIRE_FALSE);
      case 'number':
        writer.u8(WIRE_NUMBER);
        return writer.f64(item);
      case 'string':
        writer.u8(WIRE_STRING);
        return writer.string(item);
      case 'bigint':
        writer.u8(WIRE_BIGINT);
        return writer.string(String(item));
      case 'object': {
        if (active.has(item)) throw new TypeError('Cannot transfer cyclic values');
        active.add(item);
        if (Array.isArray(item)) {
          writer.u8(WIRE_ARRAY);
          writer.u32(checkedEntries(item.length, 'array'));
          for (const value of item) visit(value, depth + 1);
        } else {
          const keys = Object.keys(item);
          writer.u8(WIRE_OBJECT);
          writer.u32(checkedEntries(keys.length, 'object'));
          for (const key of keys) {
            writer.string(key);
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

export function decodeWire(bytes) {
  const reader = new WireReader(bytes);
  if (reader.u8() !== WIRE_VERSION) throw new TypeError('Invalid @antjs.org/wasm value version');

  const visit = depth => {
    if (depth > WIRE_MAX_DEPTH) throw new TypeError('Transferred value is too deeply nested');
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
        return reader.string();
      case WIRE_BIGINT:
        return BigInt(reader.string());
      case WIRE_ARRAY: {
        const length = checkedEntries(reader.u32(), 'array');
        return Array.from({ length }, () => visit(depth + 1));
      }
      case WIRE_OBJECT: {
        const length = checkedEntries(reader.u32(), 'object');
        const result = {};
        for (let index = 0; index < length; index++) {
          const key = reader.string();
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
