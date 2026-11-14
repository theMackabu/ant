const bytesToBase64 = bytes => {
  let text = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(text);
};

const base64ToBytes = text => {
  const raw = atob(text);
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
  return bytes;
};

const encode = root => {
  const seen = new Map();
  let nextId = 1;
  const visit = value => {
    if (value === undefined) return { t: 'u' };
    if (value === null) return { t: 'n' };
    if (typeof value === 'boolean') return { t: 'b', v: value };
    if (typeof value === 'number') return { t: 'd', v: Object.is(value, -0) ? '-0' : String(value) };
    if (typeof value === 'string') return { t: 's', v: value };
    if (typeof value === 'bigint') return { t: 'i', v: String(value) };
    if (typeof value === 'symbol' || typeof value === 'function') {
      throw new DOMException('Value could not be cloned', 'DataCloneError');
    }
    if (seen.has(value)) return { t: 'r', id: seen.get(value) };
    const id = nextId++;
    seen.set(value, id);
    if (Array.isArray(value)) return { t: 'a', id, v: value.map(visit) };
    if (value instanceof Date) return { t: 'date', id, v: value.toISOString() };
    if (value instanceof RegExp) return { t: 're', id, s: value.source, f: value.flags };
    if (value instanceof Map) return { t: 'map', id, v: [...value].map(([k, v]) => [visit(k), visit(v)]) };
    if (value instanceof Set) return { t: 'set', id, v: [...value].map(visit) };
    if (value instanceof ArrayBuffer) return { t: 'ab', id, v: bytesToBase64(new Uint8Array(value)) };
    if (ArrayBuffer.isView(value))
      return {
        t: 'ta',
        id,
        n: value.constructor.name,
        v: bytesToBase64(new Uint8Array(value.buffer, value.byteOffset, value.byteLength))
      };
    if (value instanceof Error) return { t: 'err', id, n: value.name, m: value.message, s: value.stack || '' };
    const props = [];
    for (const key of Object.keys(value)) props.push([key, visit(value[key])]);
    return { t: 'o', id, v: props };
  };
  return JSON.stringify({ version: 1, value: visit(root) });
};

const decode = wire => {
  const envelope = JSON.parse(wire);
  if (envelope.version !== 1) throw new Error('Unsupported Ant clone version');
  const refs = new Map();
  const visit = node => {
    if (node.t === 'u') return undefined;
    if (node.t === 'n') return null;
    if (node.t === 'b' || node.t === 's') return node.v;
    if (node.t === 'd') return node.v === '-0' ? -0 : Number(node.v);
    if (node.t === 'i') return BigInt(node.v);
    if (node.t === 'r') return refs.get(node.id);
    let value;
    if (node.t === 'a') value = [];
    else if (node.t === 'date') value = new Date(node.v);
    else if (node.t === 're') value = new RegExp(node.s, node.f);
    else if (node.t === 'map') value = new Map();
    else if (node.t === 'set') value = new Set();
    else if (node.t === 'ab') value = base64ToBytes(node.v).buffer;
    else if (node.t === 'ta') {
      const bytes = base64ToBytes(node.v);
      value = new globalThis[node.n](bytes.buffer);
    } else if (node.t === 'err') {
      const Ctor = typeof globalThis[node.n] === 'function' ? globalThis[node.n] : Error;
      value = new Ctor(node.m);
      value.stack = node.s;
    } else value = {};
    refs.set(node.id, value);
    if (node.t === 'a') node.v.forEach(item => value.push(visit(item)));
    else if (node.t === 'map') node.v.forEach(([k, v]) => value.set(visit(k), visit(v)));
    else if (node.t === 'set') node.v.forEach(item => value.add(visit(item)));
    else if (node.t === 'o') node.v.forEach(([k, v]) => (value[k] = visit(v)));
    return value;
  };
  return visit(envelope.value);
};
