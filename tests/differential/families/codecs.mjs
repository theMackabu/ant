import fc from 'fast-check';
import { finish, literal, numberSource } from '../prelude.mjs';
import { text } from './family.mjs';
import { base64Seeds, numbersSeeds, textSeeds } from './seeds.mjs';

const base64Units = [...'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', '-', '_', '=', '==', ' ', '\t', '\n', '\f', '\r', ' ', 'é', '.', '%', '#', '%41', '\u0000'];
const hexUnits = [...'0123456789abcdefABCDEF', 'g', ' ', 'x', 'é'];
const joined = (units, maxLength) => fc.array(fc.constantFrom(...units), { maxLength }).map(parts => parts.join(''));

const base64Text = fc.oneof(
  joined(base64Units, 16),
  fc.tuple(fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 10 }), fc.constantFrom('base64', 'base64url'), fc.boolean(), fc.option(fc.tuple(fc.constantFrom(...base64Units), fc.double({ min: 0, max: 1, noNaN: true }), fc.integer({ min: 0, max: 1 }))))
    .map(([bytes, alphabet, omitPadding, edit]) => {
      const encoded = Buffer.from(bytes).toString(alphabet === 'base64' ? 'base64' : 'base64url');
      const padded = omitPadding || alphabet === 'base64url' ? encoded.replace(/=+$/, '') : encoded;
      if (!edit) return padded;
      const [unit, at, remove] = edit;
      const position = Math.floor(padded.length * at);
      return padded.slice(0, position) + unit + padded.slice(position + remove);
    })
);

const base64Options = fc.record({
  alphabet: fc.constantFrom(undefined, 'base64', 'base64url'),
  lastChunkHandling: fc.constantFrom(undefined, 'loose', 'strict', 'stop-before-partial'),
  omitPadding: fc.constantFrom(undefined, false, true)
});

const base64SeedCases = base64Seeds.map(seed => ({
  bytes: seed.bytes,
  text: seed.text,
  hex: '',
  options: { alphabet: seed.url ? 'base64url' : 'base64', lastChunkHandling: seed.lastChunkHandling },
  room: seed.step * 2,
  step: seed.step
}));

const base64Probe = String.raw`
async function __recordAsync(label, callback) {
  try { __out.push([label, __canon(await callback())]); }
  catch (error) { __out.push([label, '$error:' + (error && error.name)]); }
}
const __bytes = value => Array.from(value);
const __try = callback => { try { return callback(); } catch (error) { return '$error:' + (error && error.name); } };
function __modes(bytes) {
  const out = [];
  for (const alphabet of ['base64', 'base64url'])
    for (const omitPadding of [false, true]) {
      const encoded = bytes.toBase64({ alphabet, omitPadding });
      for (const lastChunkHandling of ['loose', 'strict', 'stop-before-partial'])
        out.push([alphabet, omitPadding, lastChunkHandling, __try(() => __bytes(Uint8Array.fromBase64(encoded, { alphabet, lastChunkHandling })))]);
      out.push([alphabet, omitPadding, 'buffer', __bytes(Buffer.from(encoded, alphabet)), __try(() => __bytes(Buffer.from(atob(encoded), 'latin1')))]);
    }
  return out;
}
async function __base64Case(label, bytesList, text, hex, options, room, step) {
  const bytes = new Uint8Array(bytesList);
  __recordName('encode:' + label, () => [bytes.toBase64(), bytes.toBase64(options), bytes.toHex()]);
  __recordName('decode:' + label, () => __bytes(Uint8Array.fromBase64(text, options)));
  __recordName('decode-url:' + label, () => __bytes(Uint8Array.fromBase64(text, Object.assign({}, options, { alphabet: 'base64url' }))));
  __recordName('into:' + label, () => { const target = new Uint8Array(room).fill(238); const result = target.setFromBase64(text, options); return [result.read, result.written, __bytes(target)]; });
  __recordName('resume:' + label, () => { const out = []; let offset = 0; for (let guard = 0; guard < 64; guard++) { const target = new Uint8Array(step); const result = target.setFromBase64(text.slice(offset), options); out.push(...target.subarray(0, result.written)); offset += result.read; if (result.read === 0 || result.written < step) break; } return [offset, out]; });
  __recordName('roundtrip:' + label, () => __bytes(Uint8Array.fromBase64(bytes.toBase64(options), options)));
  __recordName('modes:' + label, () => __modes(bytes));
  __recordName('decoders:' + label, () => [__try(() => __bytes(Buffer.from(text, 'base64'))), __try(() => __bytes(Buffer.from(text, 'base64url'))), __try(() => __bytes(Buffer.from(atob(text), 'latin1'))), __try(() => __bytes(Uint8Array.fromBase64(text, { lastChunkHandling: 'loose' })))]);
  await __recordAsync('data-url:' + label, async () => __bytes(new Uint8Array(await (await fetch('data:application/octet-stream;base64,' + text)).arrayBuffer())));
  __recordName('hex:' + label, () => __bytes(Uint8Array.fromHex(hex)));
  __recordName('hex-into:' + label, () => { const target = new Uint8Array(room).fill(238); const result = target.setFromHex(hex); return [result.read, result.written, __bytes(target)]; });
}`;

export const base64 = {
  name: 'base64',
  arbitrary: fc.record({
    inputs: fc.array(
      fc.record({
        bytes: fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 12 }),
        text: base64Text,
        hex: joined(hexUnits, 12),
        options: base64Options,
        room: fc.integer({ min: 0, max: 9 }),
        step: fc.integer({ min: 1, max: 5 })
      }),
      { minLength: 1, maxLength: 5 }
    )
  }),
  build({ inputs }) {
    const call = prefix => ({ bytes, text, hex, options, room, step }, i) =>
      `await __base64Case(${literal(prefix + i)}, ${literal(bytes)}, ${literal(text)}, ${literal(hex)}, ${literal(options)}, ${room}, ${step});`;
    const first = literal(inputs[0].text);
    const fixed = `__recordName('option-order', () => { const log = []; const options = {}; for (const key of ['alphabet', 'lastChunkHandling', 'omitPadding']) Object.defineProperty(options, key, { get() { log.push(key); return undefined; } }); Uint8Array.fromBase64('', options); new Uint8Array(1).toBase64(options); new Uint8Array(1).setFromBase64('', options); return log; });
__recordName('bad-alphabet', () => Uint8Array.fromBase64('', { alphabet: 'base32' }));
__recordName('bad-last-chunk', () => Uint8Array.fromBase64('', { lastChunkHandling: 'lax' }));
__recordName('non-string', () => Uint8Array.fromBase64(12));
__recordName('non-string-hex', () => Uint8Array.fromHex(12));
__recordName('subarray', () => { const whole = new Uint8Array(6).fill(1); const result = whole.subarray(2, 4).setFromBase64(${first}); return [result.read, result.written, Array.from(whole)]; });
__recordName('detached', () => { const target = new Uint8Array(4); structuredClone(target.buffer, { transfer: [target.buffer] }); return target.setFromBase64(${first}); });
__recordName('wrong-receiver', () => Uint8Array.prototype.toBase64.call(new Int8Array(2)));`;
    return finish([base64Probe, fixed, ...inputs.map(call('')), ...base64SeedCases.map(call('seed:'))].join('\n'), true);
  }
};

const view = new DataView(new ArrayBuffer(8));

function doubleFromBits(bits) {
  view.setBigUint64(0, bits);
  return view.getFloat64(0);
}

function decompose(value) {
  view.setFloat64(0, value);
  const bits = view.getBigUint64(0);
  const exponentBits = Number((bits >> 52n) & 0x7ffn);
  const fraction = bits & ((1n << 52n) - 1n);
  return exponentBits === 0 ? [fraction, -1074] : [fraction | (1n << 52n), exponentBits - 1075];
}

function exactDecimal(mantissa, exponent) {
  if (exponent >= 0) return (mantissa << BigInt(exponent)).toString();
  const digits = (mantissa * 5n ** BigInt(-exponent)).toString().padStart(-exponent + 1, '0');
  const point = digits.length + exponent;
  const fraction = digits.slice(point).replace(/0+$/, '');
  return fraction ? `${digits.slice(0, point)}.${fraction}` : digits.slice(0, point);
}

function nudge(decimal, direction) {
  if (!decimal.includes('.')) return (BigInt(decimal) + BigInt(direction)).toString();
  return direction > 0 ? `${decimal}000000000000000000001` : `${decimal.slice(0, -1)}${Number(decimal.at(-1)) - 1}99999999999999999999`;
}

function decimalFor(value, kind) {
  const [mantissa, exponent] = decompose(value);
  if (kind === 'exact') return exactDecimal(mantissa, exponent);
  const midpoint = exactDecimal(mantissa * 2n + 1n, exponent - 1);
  if (kind === 'midpoint') return midpoint;
  return nudge(midpoint, kind === 'above' ? 1 : -1);
}

function scientific(decimal, shift) {
  const [whole, fraction = ''] = decimal.split('.');
  const digits = (whole + fraction).replace(/^0+/, '') || '0';
  const pointAt = whole === '0' ? -(fraction.length - fraction.replace(/^0+/, '').length) : whole.length;
  const lead = Math.max(1, Math.min(digits.length, 1 + shift));
  const mantissa = digits.length > lead ? `${digits.slice(0, lead)}.${digits.slice(lead)}` : digits;
  return `${mantissa}e${pointAt - lead}`;
}

const positiveDouble = fc.oneof(
  { weight: 3, arbitrary: fc.bigInt({ min: 1n, max: 0x7fefffffffffffffn }).map(doubleFromBits) },
  { weight: 1, arbitrary: fc.bigInt({ min: 1n, max: 0x000fffffffffffffn }).map(doubleFromBits) },
  { weight: 1, arbitrary: fc.constantFrom(5e-324, 2.2250738585072009e-308, 2.2250738585072014e-308, 1e23, 9007199254740993, 2 ** 53, 0.1, 1 / 3, Number.MAX_VALUE, 1e21, 1e-7, 123456789012345680000) }
);

const numericForms = ['plain', 'scientific', 'shifted', 'uppercase'];
const junkUnits = ['0', '1', '9', '.', 'e', 'E', '+', '-', 'x', 'X', 'b', 'o', '_', 'n', ' ', '\t', '\n', ' ', '﻿', ' ', '　', 'Infinity', 'NaN', 'a', 'f'];

const doubleRecords = (x, digits, label) => `__record(${literal(`format:${label}`)}, () => [String(${x}), (${x}).toExponential(), (${x}).toString(2).length]);
__record(${literal(`roundtrip:${label}`)}, () => { const v = ${x}; const same = s => v === 0 ? Number(s) === 0 : Object.is(Number(s), v); return [same(String(v)), same(v.toPrecision(17)), same(v.toExponential(16))]; });
__recordName(${literal(`digits:${label}`)}, () => [(${x}).toPrecision(${Math.max(1, digits % 101)}), (${x}).toExponential(${digits % 101}), (${x}).toFixed(${digits % 101})]);`;

const stringRecords = (s, label) => `__record(${literal(`Number:${label}`)}, () => Number(${s}));
__record(${literal(`parseFloat:${label}`)}, () => parseFloat(${s}));
__record(${literal(`parseInt:${label}`)}, () => [parseInt(${s}), parseInt(${s}, 16), parseInt(${s}, 2)]);
__recordName(${literal(`JSON:${label}`)}, () => JSON.parse(${s}));
__recordName(${literal(`BigInt:${label}`)}, () => String(BigInt(${s})));
__record(${literal(`trim:${label}`)}, () => [${s}.trim(), ${s}.trimStart().length, ${s}.trimEnd().length]);`;

export const numberParse = {
  name: 'number-parse',
  arbitrary: fc.record({
    values: fc.array(
      fc.record({
        value: positiveDouble,
        kind: fc.constantFrom('exact', 'midpoint', 'below', 'above'),
        form: fc.constantFrom(...numericForms),
        shift: fc.integer({ min: 0, max: 30 }),
        prefix: fc.constantFrom('', '', '-', '+', ' ', '\n\t', '00', ' ', '﻿'),
        suffix: fc.constantFrom('', '', ' ', 'e0', ' ', 'x', '_1'),
        digits: fc.integer({ min: 0, max: 100 })
      }),
      { minLength: 1, maxLength: 6 }
    ),
    junk: fc.array(fc.oneof(joined(junkUnits, 8), text(6)), { maxLength: 3 })
  }),
  build({ values, junk }) {
    const generated = values.map(({ value, kind, form, shift, prefix, suffix, digits }, i) => {
      const decimal = decimalFor(value, kind);
      let body = form === 'plain' ? decimal : scientific(decimal, form === 'shifted' ? shift : 0);
      if (form === 'uppercase') body = body.toUpperCase();
      return stringRecords(literal(prefix + body + suffix), String(i)) + '\n' + doubleRecords(numberSource(value), digits, String(i));
    });
    const junkRecords = junk.map((s, i) => stringRecords(literal(s), `junk:${i}`));
    const seeds = numbersSeeds.map(seed => stringRecords(literal(seed.text), `seed:${seed.name}`) + '\n' + doubleRecords(numberSource(doubleFromBits(BigInt(seed.bits))), seed.digits, `seed:${seed.name}`));
    return finish([...generated, ...junkRecords, ...seeds].join('\n'));
  }
};

const byteSegments = [
  [0x61], [0x7f], [0xc3, 0xa9], [0xe2, 0x82, 0xac], [0xf0, 0x9f, 0x98, 0x80], [0xef, 0xbb, 0xbf], [0x80], [0xbf], [0xc0, 0x80],
  [0xc2], [0xe2, 0x82], [0xf0, 0x9f, 0x98], [0xed, 0xa0, 0x80], [0xed, 0xbf, 0xbf], [0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80],
  [0xf4, 0x90, 0x80, 0x80], [0xf8], [0xff], [0x00], [0x25], [0x2b], [0x26], [0x3d], [0x5c]
];
const textBytes = fc.oneof(
  { weight: 3, arbitrary: fc.array(fc.constantFrom(...byteSegments), { maxLength: 8 }).map(segments => segments.flat()) },
  { weight: 1, arbitrary: fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 12 }) }
);
const percentUnits = ['%', '%4', '%41', '%zz', '%e2%82', '%e2%82%ac', '%ff', '%C3%A9', '+', '&', '=', 'a', '%00', '%ED%A0%80', '%F0%9F%98%80', '%2', ' ', 'é'];
const quotes = ['"', "'", '`', ''];

const textProbe = String.raw`
function __textCase(label, bytesList, s, quote) {
  const u8 = new Uint8Array(bytesList);
  const utf8 = new TextDecoder().decode(u8);
  __recordName('utf8:' + label, () => [Buffer.from(u8).toString('utf8'), utf8, utf8.isWellFormed()]);
  __recordName('fatal:' + label, () => new TextDecoder('utf-8', { fatal: true }).decode(u8));
  __recordName('fatal-bom:' + label, () => new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(u8));
  __recordName('bom:' + label, () => [new TextDecoder('utf-8', { ignoreBOM: true }).decode(u8), new TextDecoder().decode(u8.subarray(0, 3))]);
  __recordName('latin1:' + label, () => { const l = Buffer.from(u8).toString('latin1'); return [l, Array.from(Buffer.from(l, 'latin1')), new TextDecoder('latin1').decode(u8)]; });
  for (const [name, value] of [['string', s], ['bytes', utf8]]) {
    const at = name + ':' + label;
    __recordName('wtf:' + at, () => [value.isWellFormed(), value.toWellFormed(), value.length, Array.from(value, c => c.codePointAt(0))]);
    __recordName('join:' + at, () => Array.from({ length: value.length + 1 }, (_, k) => value.slice(0, k) + value.slice(k) === value && (value.slice(0, k) + value.slice(k)).length === value.length).every(Boolean));
    __recordName('json-quote:' + at, () => [JSON.stringify(value), JSON.parse(JSON.stringify(value)) === value]);
    __recordName('form:' + at, () => { const p = new URLSearchParams([[value, value]]); const t = p.toString(); return [t, Array.from(new URLSearchParams(t))]; });
    __recordName('form-decode:' + at, () => Array.from(new URLSearchParams(value)));
    __recordName('uri-component:' + at, () => decodeURIComponent(value));
    __recordName('uri:' + at, () => decodeURI(value));
    __recordName('literal:' + at, () => new Function('const a = 1; return ' + (quote || '"') + value + (quote || '"'))());
  }
}`;

export const textFamily = {
  name: 'text',
  arbitrary: fc.record({
    inputs: fc.array(
      fc.record({
        bytes: textBytes,
        s: fc.oneof(text(8), joined(percentUnits, 6)),
        quote: fc.constantFrom(...quotes)
      }),
      { minLength: 1, maxLength: 4 }
    )
  }),
  build({ inputs }) {
    const generated = inputs.map(({ bytes, s, quote }, i) => `__textCase(${literal(String(i))}, ${literal(bytes)}, ${literal(s)}, ${literal(quote)});`);
    const seeds = textSeeds.map(seed => `__textCase(${literal(`seed:${seed.name}`)}, ${literal(seed.bytes)}, ${literal(Buffer.from(seed.bytes).toString('latin1'))}, ${literal(seed.quote)});`);
    return finish([textProbe, ...generated, ...seeds].join('\n'));
  }
};
