const VIEW_TYPES = [
  Int8Array,
  Uint8Array,
  Uint8ClampedArray,
  Int16Array,
  Uint16Array,
  Int32Array,
  Uint32Array,
  Float32Array,
  Float64Array,
  DataView,
  Uint8Array,
  BigInt64Array,
  BigUint64Array,
  globalThis.Float16Array
];
const VIEW_NAMES = VIEW_TYPES.map((type, index) => (index === 10 || !type ? '' : type.name));
const VIEW_TAGS = { b: 0, B: 1, C: 2, w: 3, W: 4, d: 5, D: 6, f: 7, F: 8, '?': 9, q: 11, Q: 12, h: 13 };
const ERROR_TYPES = { E: EvalError, R: RangeError, F: ReferenceError, S: SyntaxError, T: TypeError, U: URIError };
const REGEXP_FLAGS = { d: 128, g: 1, i: 2, m: 4, s: 32, u: 16, v: 256, y: 8 };
const INDEX_KEY = /^(?:0|[1-9]\d{0,9})$/;
const tag = char => char.charCodeAt(0);

const encode = root => {
  let bytes = new Uint8Array(256);
  let view = new DataView(bytes.buffer);
  let length = 0;
  const ids = new Map();

  const reserve = count => {
    if (length + count > bytes.length) {
      const grown = new Uint8Array(Math.max(bytes.length * 2, length + count));
      grown.set(bytes);
      bytes = grown;
      view = new DataView(bytes.buffer);
    }
    length += count;
    return length - count;
  };
  const byte = value => (bytes[reserve(1)] = value);
  const raw = source => bytes.set(source, reserve(source.length));
  const double = value => view.setFloat64(reserve(8), value, true);
  const varint = value => {
    for (value = BigInt(value); value > 0x7fn; value >>= 7n) byte(Number(value & 0x7fn) | 0x80);
    byte(Number(value));
  };

  const number = value => {
    if (Number.isInteger(value) && value >= -0x80000000 && value <= 0x7fffffff && !Object.is(value, -0)) {
      byte(tag('I'));
      varint(((value << 1) ^ (value >> 31)) >>> 0);
    } else {
      byte(tag('N'));
      double(value);
    }
  };

  const string = value => {
    let oneByte = true;
    for (let i = 0; i < value.length && oneByte; i++) oneByte = value.charCodeAt(i) <= 0xff;
    const size = oneByte ? value.length : value.length * 2;
    let prefix = 1;
    for (let rest = size; rest > 0x7f; rest = Math.floor(rest / 128)) prefix++;
    if (!oneByte && (length + 1 + prefix) & 1) byte(0);
    byte(tag(oneByte ? '"' : 'c'));
    varint(size);
    const offset = reserve(size);
    for (let i = 0; i < value.length; i++) {
      if (oneByte) bytes[offset + i] = value.charCodeAt(i);
      else view.setUint16(offset + i * 2, value.charCodeAt(i), true);
    }
  };

  const bigint = value => {
    const negative = value < 0n;
    let magnitude = negative ? -value : value;
    const words = [];
    for (; magnitude > 0n; magnitude >>= 64n) words.push(magnitude & 0xffffffffffffffffn);
    varint(words.length * 16 + (negative ? 1 : 0));
    for (const word of words) view.setBigUint64(reserve(8), word, true);
  };

  const properties = (object, skipIndices) => {
    let count = 0;
    for (const key of Object.keys(object)) {
      const index = INDEX_KEY.test(key) && Number(key) < 0xffffffff;
      if (index && skipIndices) continue;
      if (index) number(Number(key));
      else string(key);
      write(object[key]);
      count++;
    }
    return count;
  };

  const object = value => {
    if (typeof value === 'function') throw new DOMException(`${value} could not be cloned.`, 'DataCloneError');
    if (ids.has(value)) {
      byte(tag('^'));
      return varint(ids.get(value));
    }
    ids.set(value, ids.size);

    if (ArrayBuffer.isView(value)) {
      byte(tag('\\'));
      varint(VIEW_NAMES.indexOf(Object.prototype.toString.call(value).slice(8, -1)));
      varint(value.byteLength);
      return raw(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
    }
    if (value instanceof ArrayBuffer) {
      byte(tag('B'));
      varint(value.byteLength);
      return raw(new Uint8Array(value));
    }
    if (value instanceof Date) {
      byte(tag('D'));
      return double(value.getTime());
    }
    if (value instanceof RegExp) {
      byte(tag('R'));
      string(value.source);
      return varint([...value.flags].reduce((bits, flag) => bits | REGEXP_FLAGS[flag], 0));
    }
    if (value instanceof Boolean) return byte(tag(value.valueOf() ? 'y' : 'x'));
    if (value instanceof Number) {
      byte(tag('n'));
      return double(value.valueOf());
    }
    if (value instanceof BigInt) {
      byte(tag('z'));
      return bigint(value.valueOf());
    }
    if (value instanceof String) {
      byte(tag('s'));
      return string(value.valueOf());
    }
    if (value instanceof Error) {
      byte(tag('r'));
      const prototype = Object.keys(ERROR_TYPES).find(key => value instanceof ERROR_TYPES[key]);
      if (prototype) byte(tag(prototype));
      if (Object.hasOwn(value, 'message')) {
        byte(tag('m'));
        string(String(value.message));
      }
      if (typeof value.stack === 'string') {
        byte(tag('s'));
        string(value.stack);
      }
      if (Object.hasOwn(value, 'cause')) {
        byte(tag('c'));
        write(value.cause);
      }
      return byte(tag('.'));
    }
    if (value instanceof Map) {
      byte(tag(';'));
      for (const [key, item] of value) {
        write(key);
        write(item);
      }
      byte(tag(':'));
      return varint(value.size * 2);
    }
    if (value instanceof Set) {
      byte(tag("'"));
      for (const item of value) write(item);
      byte(tag(','));
      return varint(value.size);
    }
    if (Array.isArray(value)) {
      let dense = true;
      for (let i = 0; i < value.length && dense; i++) dense = i in value;
      byte(tag(dense ? 'A' : 'a'));
      varint(value.length);
      if (dense) for (const item of value) write(item);
      const count = properties(value, dense);
      byte(tag(dense ? '$' : '@'));
      varint(count);
      return varint(value.length);
    }
    byte(tag('o'));
    const count = properties(value, false);
    byte(tag('{'));
    varint(count);
  };

  const write = value => {
    if (value === undefined) byte(tag('_'));
    else if (value === null) byte(tag('0'));
    else if (value === true) byte(tag('T'));
    else if (value === false) byte(tag('F'));
    else if (typeof value === 'number') number(value);
    else if (typeof value === 'string') string(value);
    else if (typeof value === 'bigint') {
      byte(tag('Z'));
      bigint(value);
    } else if (typeof value === 'symbol') throw new DOMException('Symbol could not be cloned.', 'DataCloneError');
    else object(value);
  };

  byte(0xff);
  byte(15);
  write(root);
  return bytes.slice(0, length);
};

const decode = bytes => {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const refs = [];
  let position = 0;

  const fail = () => {
    throw new Error('Unable to deserialize cloned data.');
  };
  const take = count => {
    if (position + count > bytes.length) fail();
    position += count;
    return position - count;
  };
  const peek = () => {
    while (bytes[position] === 0) position++;
    return position < bytes.length ? String.fromCharCode(bytes[position]) : fail();
  };
  const varint = () => {
    let value = 0n;
    for (let shift = 0n; ; shift += 7n) {
      const next = bytes[take(1)];
      value |= BigInt(next & 0x7f) << shift;
      if (!(next & 0x80)) return Number(value);
    }
  };
  const double = () => view.getFloat64(take(8), true);
  const copy = (offset, size) => bytes.buffer.slice(bytes.byteOffset + offset, bytes.byteOffset + offset + size);
  const track = value => (refs.push(value), value);

  const string = kind => {
    const size = varint();
    const offset = take(size);
    if (kind === 'S') return new TextDecoder().decode(bytes.subarray(offset, offset + size));
    let text = '';
    const step = kind === 'c' ? 2 : 1;
    for (let i = 0; i < size; i += step) {
      text += String.fromCharCode(step === 2 ? view.getUint16(offset + i, true) : bytes[offset + i]);
    }
    return text;
  };

  const bigint = () => {
    const bitfield = varint();
    const size = bitfield >> 1;
    const offset = take(size);
    let value = 0n;
    for (let i = size - 1; i >= 0; i--) value = (value << 8n) | BigInt(bytes[offset + i]);
    return bitfield & 1 ? -value : value;
  };

  const properties = (target, end) => {
    let count = 0;
    for (; peek() !== end; count++) {
      const key = read();
      target[key] = read();
    }
    position++;
    if (varint() !== count) fail();
    return target;
  };

  const viewOf = buffer => {
    if (!(buffer instanceof ArrayBuffer) || position >= bytes.length || peek() !== 'V') return buffer;
    position++;
    const type = VIEW_TYPES[VIEW_TAGS[String.fromCharCode(bytes[take(1)])]] || fail();
    const offset = varint();
    const size = varint();
    varint();
    return track(new type(buffer, offset, size / (type.BYTES_PER_ELEMENT || 1)));
  };

  const read = () => {
    const kind = peek();
    position++;
    switch (kind) {
      case '_': return undefined;
      case '0': return null;
      case 'T': return true;
      case 'F': return false;
      case 'I': {
        const value = varint();
        return (value >>> 1) ^ -(value & 1);
      }
      case 'U': return varint();
      case 'N': return double();
      case 'Z': return bigint();
      case 'S':
      case '"':
      case 'c': return string(kind);
      case '^': return viewOf(refs[varint()] ?? fail());
      case 'o': return properties(track({}), '{');
      case 'A': {
        const array = track(new Array(varint()));
        for (let i = 0; i < array.length; i++) {
          if (peek() === '-') position++;
          else array[i] = read();
        }
        properties(array, '$');
        return varint() === array.length ? array : fail();
      }
      case 'a': {
        const array = properties(track(new Array(varint())), '@');
        return varint() === array.length ? array : fail();
      }
      case 'D': return track(new Date(double()));
      case 'R': {
        const source = read();
        const bits = varint();
        return track(new RegExp(source, Object.keys(REGEXP_FLAGS).filter(flag => bits & REGEXP_FLAGS[flag]).join('')));
      }
      case 'y': return track(Object(true));
      case 'x': return track(Object(false));
      case 'n': return track(Object(double()));
      case 'z': return track(Object(bigint()));
      case 's': return track(Object(read()));
      case ';': {
        const map = track(new Map());
        while (peek() !== ':') map.set(read(), read());
        position++;
        return varint() === map.size * 2 ? map : fail();
      }
      case "'": {
        const set = track(new Set());
        while (peek() !== ',') set.add(read());
        position++;
        return varint() === set.size ? set : fail();
      }
      case 'r': {
        let field = String.fromCharCode(bytes[take(1)]);
        const type = ERROR_TYPES[field] || Error;
        if (type !== Error) field = String.fromCharCode(bytes[take(1)]);
        const error = track(new type());
        for (; field !== '.'; field = String.fromCharCode(bytes[take(1)])) {
          const name = { m: 'message', s: 'stack', c: 'cause' }[field] || fail();
          Object.defineProperty(error, name, { value: read(), writable: true, configurable: true });
        }
        return error;
      }
      case 'B': {
        const size = varint();
        const offset = take(size);
        return viewOf(track(copy(offset, size)));
      }
      case '\\': {
        const type = VIEW_TYPES[varint()] || fail();
        const size = varint();
        const offset = take(size);
        const buffer = copy(offset, size);
        return track(new type(buffer, 0, size / (type.BYTES_PER_ELEMENT || 1)));
      }
      default: return fail();
    }
  };

  if (bytes[take(1)] !== 0xff || bytes[take(1)] < 13 || bytes[position - 1] > 15) fail();
  return read();
};
