// Property families with Node as the oracle: random inputs for built-ins,
// recording what they return. Error messages are engine-specific, so these
// probes record only error names (__recordName).

import fc from 'fast-check';
import { finish, literal, numberSource } from '../prelude.ts';
import { defineFamily, doubleValue, text } from './family.ts';

const keyPool = ['a', 'b', '', '0', '1', '01', '-1', '1.5', '__proto__', 'constructor', 'toJSON', 'é', '😀', 'a\u0000b', '\uD800'];
const objectKey = fc.oneof({ weight: 3, arbitrary: fc.constantFrom(...keyPool) }, { weight: 1, arbitrary: text(3) });

const jsonLeaf = fc.oneof(
  doubleValue.map(numberSource),
  text(6).map(literal),
  fc.constantFrom('true', 'false', 'null', 'undefined', '[]', '{}', '10n', 'new Date(0)', '() => 1', 'Symbol("s")')
);

// Source text for a JSON-ish value tree.
const { jsonValue } = fc.letrec<{ jsonValue: string }>(tie => ({
  jsonValue: fc.oneof(
    { maxDepth: 3, depthIdentifier: 'json' },
    jsonLeaf,
    fc.array(tie('jsonValue'), { maxLength: 4 }).map(items => `[${items.join(', ')}]`),
    // unique keys: a repeated "__proto__" in a literal is a SyntaxError
    fc.uniqueArray(fc.tuple(objectKey, tie('jsonValue')), { maxLength: 4, selector: ([k]) => k })
      .map(entries => `{ ${entries.map(([k, v]) => `${literal(k)}: ${v}`).join(', ')} }`)
  )
}));

const jsonEdits = [',', ']', '}', '"', '\\', '0', '-', 'e', ' ', ':', '\u2028', 'x', '{', '['];

export const json = defineFamily({
  name: 'json',
  arbitrary: fc.record({
    values: fc.array(
      fc.record({
        source: jsonValue,
        edits: fc.array(fc.tuple(fc.constantFrom(...jsonEdits), fc.double({ min: 0, max: 1, noNaN: true })), { maxLength: 3 }),
        indent: fc.constantFrom(0, 1, 2, 4, 10, 11, '\t', '--', '')
      }),
      { minLength: 1, maxLength: 5 }
    )
  }),
  build({ values }) {
    return finish(
      values
        .map(({ source, edits, indent }, i) => {
          const editSource = edits
            .map(([edit, at], k) => `t = t.slice(0, Math.floor(t.length * ${at})) + ${literal(edit)} + t.slice(Math.floor(t.length * ${at}) + ${k % 2});`)
            .join(' ');
          return `{ const v = ${source};
__recordName(${literal(`stringify:${i}`)}, () => JSON.stringify(v));
__recordName(${literal(`indent:${i}`)}, () => JSON.stringify(v, null, ${literal(indent)}));
__recordName(${literal(`keys:${i}`)}, () => JSON.stringify(v, ['a', '0', '__proto__']));
__recordName(${literal(`replacer:${i}`)}, () => JSON.stringify(v, (k, x) => typeof x === 'number' ? -x : x));
__recordName(${literal(`roundtrip:${i}`)}, () => JSON.parse(JSON.stringify(v) ?? 'null'));
__recordName(${literal(`reviver:${i}`)}, () => JSON.parse(JSON.stringify(v) ?? '0', (k, x) => typeof x === 'string' ? x.length : x));
__recordName(${literal(`edited:${i}`)}, () => { let t = JSON.stringify(v) ?? ''; ${editSource} return [t, JSON.parse(t)]; }); }`;
        })
        .join('\n')
    );
  }
});

const exactRadix = (radix: number | undefined): boolean => radix === undefined || [0, 2, 4, 8, 10, 16, 32].includes(radix);

const numericStringParts = [
  '', ' ', '\t', '\n', '\u00a0', '\ufeff', '\u2028', '\u3000', '\u180e', '+', '-', '0', '1', '7', '9', '00', '0x', '0X', '0b', '0o',
  'f', 'F', '.', 'e', 'E', 'e+', 'e-', '5', '308', '400', 'Infinity', 'infinity', 'NaN', '_', '1_0', 'n', 'x', ','
];

export const number = defineFamily({
  name: 'number',
  arbitrary: fc.record({
    doubles: fc.array(
      fc.record({
        value: doubleValue,
        radix: fc.integer({ min: 2, max: 36 }),
        fixed: fc.integer({ min: 0, max: 100 }),
        precision: fc.integer({ min: 1, max: 100 }),
        exponential: fc.integer({ min: 0, max: 100 })
      }),
      { minLength: 1, maxLength: 8 }
    ),
    strings: fc.array(
      fc.record({
        text: fc.array(fc.constantFrom(...numericStringParts), { minLength: 1, maxLength: 5 }).map(parts => parts.join('')),
        radix: fc.constantFrom(undefined, 0, 2, 8, 10, 16, 36, 37)
      }),
      { minLength: 1, maxLength: 8 }
    )
  }),
  build({ doubles, strings }) {
    const formatted = doubles.map(({ value, radix, fixed, precision, exponential }, i) => {
      const x = numberSource(value);
      // toString(radix) is implementation-approximated; Ant uses V8's digits
      return `__record(${literal(`string:${i}`)}, () => String(${x}));
__record(${literal(`radix:${i}`)}, () => (${x}).toString(${radix}));
__recordName(${literal(`fixed:${i}`)}, () => (${x}).toFixed(${fixed}));
__recordName(${literal(`precision:${i}`)}, () => (${x}).toPrecision(${precision}));
__recordName(${literal(`exponential:${i}`)}, () => (${x}).toExponential(${exponential}));
__record(${literal(`int32:${i}`)}, () => [(${x}) | 0, (${x}) >>> 0, Math.round(${x}), Math.trunc(${x}), Math.fround(${x})]);`;
    });
    const parsed = strings.map(({ text, radix }, i) => {
      const s = literal(text);
      return `__record(${literal(`Number:${i}`)}, () => Number(${s}));
__record(${literal(`parseFloat:${i}`)}, () => parseFloat(${s}));
__record(${literal(`parseInt:${i}`)}, () => ${exactRadix(radix) ? '' : 'approximate'}(parseInt(${s}, ${radix === undefined ? 'undefined' : radix})));
__recordName(${literal(`BigInt:${i}`)}, () => String(BigInt(${s})));`;
    });
    // parseInt may approximate past 2^53 unless the radix is 10 or a power of two
    const helper = 'function approximate(v) { return Math.abs(v) > 2 ** 53 ? "$approximate" : v; }';
    return finish([helper, ...formatted, ...parsed].join('\n'));
  }
});

const index = fc.integer({ min: -2, max: 10 });

export const string = defineFamily({
  name: 'string',
  arbitrary: fc.record({
    pairs: fc.array(fc.record({ s: text(8), t: text(2), i: index, j: index, n: fc.integer({ min: 0, max: 12 }) }), { minLength: 1, maxLength: 5 })
  }),
  build({ pairs }) {
    return finish(
      pairs
        .map(({ s, t, i, j, n }, k) => `{ const s = ${literal(s)}, t = ${literal(t)};
__record(${literal(`units:${k}`)}, () => [s.length, Array.from({ length: s.length + 1 }, (_, x) => s.charCodeAt(x)), Array.from({ length: s.length + 1 }, (_, x) => s.codePointAt(x))]);
__record(${literal(`chars:${k}`)}, () => [[...s], s.at(${i}), s.charAt(${i}), s[${i}]]);
__record(${literal(`slices:${k}`)}, () => [s.slice(${i}, ${j}), s.substring(${i}, ${j}), s.substr(${i}, ${j})]);
__record(${literal(`search:${k}`)}, () => [s.indexOf(t), s.indexOf(t, ${i}), s.lastIndexOf(t), s.lastIndexOf(t, ${j}), s.includes(t), s.startsWith(t, ${i}), s.endsWith(t, ${j})]);
__record(${literal(`split:${k}`)}, () => [s.split(t), s.split(''), s.split(t, 2)]);
__record(${literal(`replace:${k}`)}, () => [s.replace(t, '[$&]'), s.replaceAll(t, '<$$>'), s.replaceAll('', '.')]);
__record(${literal(`pad:${k}`)}, () => [s.padStart(${n}, t), s.padEnd(${n}, t), s.repeat(2), s.concat(t, s)]);
__record(${literal(`trim:${k}`)}, () => [s.trim(), s.trimStart(), s.trimEnd()]);
__record(${literal(`case:${k}`)}, () => [s.toUpperCase(), s.toLowerCase()]);
__record(${literal(`normalize:${k}`)}, () => ['NFC', 'NFD', 'NFKC', 'NFKD'].map(f => s.normalize(f)));
__record(${literal(`wellformed:${k}`)}, () => [s.isWellFormed(), s.toWellFormed()]);
__recordName(${literal(`uri:${k}`)}, () => [encodeURIComponent(s), encodeURI(s)]);
__recordName(${literal(`uri-roundtrip:${k}`)}, () => decodeURIComponent(encodeURIComponent(s)) === s);
__record(${literal(`escape:${k}`)}, () => [escape(s), unescape(escape(s)) === s]);
__record(${literal(`compare:${k}`)}, () => [s < t, s === t, s > t, [s, t, s + t].sort()]);
__record(${literal(`json:${k}`)}, () => [JSON.stringify(s), JSON.parse(JSON.stringify(s)) === s]);
__record(${literal(`codes:${k}`)}, () => [String.fromCharCode(...Array.from(s, c => c.charCodeAt(0))) === s, Array.from(s, c => c.codePointAt(0))]); }`)
        .join('\n')
    );
  }
});

const cloneLeaf = fc.oneof(
  doubleValue.map(numberSource),
  text(4).map(literal),
  fc.constantFrom(
    'true', 'null', 'undefined', '12345678901234567890n', 'new Date(1e12)', 'new Date(NaN)', '/a+b/giu', 'new Number(-0)',
    'new String("s")', 'new Boolean(false)', 'Object(1n)', 'new Uint8Array([1, 2, 255])', 'new Int16Array([-1, 300])',
    'new Float64Array([NaN, -0, 1.5])', 'new BigInt64Array([-1n])', 'new DataView(new ArrayBuffer(4), 1, 2)', 'new ArrayBuffer(3)',
    'new TypeError("t")', 'new RangeError("r", { cause: 1 })', 'new Error("e")', '() => 1', 'Symbol("x")', '[, 1, , 2]',
    'Object.create(null)'
  )
);

// Source text for a value structuredClone can (or cannot) copy.
const { cloneValue } = fc.letrec<{ cloneValue: string }>(tie => ({
  cloneValue: fc.oneof(
    { maxDepth: 3, depthIdentifier: 'clone' },
    cloneLeaf,
    fc.array(tie('cloneValue'), { maxLength: 3 }).map(items => `[${items.join(', ')}]`),
    fc.array(fc.tuple(fc.oneof(fc.constantFrom('k', '1'), tie('cloneValue')), tie('cloneValue')), { maxLength: 3 })
      .map(entries => `new Map([${entries.map(([k, v]) => `[${k === 'k' || k === '1' ? literal(k) : k}, ${v}]`).join(', ')}])`),
    fc.array(tie('cloneValue'), { maxLength: 3 }).map(items => `new Set([${items.join(', ')}])`),
    fc.uniqueArray(fc.tuple(objectKey, tie('cloneValue')), { maxLength: 3, selector: ([k]) => k })
      .map(entries => `{ ${entries.map(([k, v]) => `${literal(k)}: ${v}`).join(', ')} }`)
  )
}));

// Fills objects by assignment rather than with literals (see prelude.ts).
const describeSource = String.raw`
function __describe(value, ids = new Map()) {
  if (typeof value !== 'object' && typeof value !== 'function') return __canon(value);
  if (value === null) return null;
  if (ids.has(value)) return '$ref:' + ids.get(value);
  const id = ids.size;
  ids.set(value, id);
  const out = {};
  out.id = id;
  out.tag = Object.prototype.toString.call(value);
  out.proto = Object.getPrototypeOf(value)?.constructor?.name ?? null;
  if (value instanceof Map) out.entries = [...value].map(([k, v]) => [__describe(k, ids), __describe(v, ids)]);
  else if (value instanceof Set) out.values = [...value].map(v => __describe(v, ids));
  else if (value instanceof Date) out.time = __canon(value.getTime());
  else if (value instanceof RegExp) out.regexp = [value.source, value.flags, value.lastIndex];
  else if (value instanceof Error) out.error = [value.name, value.message, 'cause' in value ? __describe(value.cause, ids) : '$none'];
  else if (ArrayBuffer.isView(value)) out.view = [value.byteOffset, value.byteLength, Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength))];
  else if (value instanceof ArrayBuffer) out.bytes = Array.from(new Uint8Array(value));
  else if (value instanceof Number || value instanceof String || value instanceof Boolean || value instanceof BigInt)
    out.primitive = __canon(value.valueOf());
  if (Array.isArray(value)) out.length = value.length;
  // stack text is engine-specific
  out.own = Reflect.ownKeys(value).filter(k => typeof k === 'string' && k !== 'stack' && !(ArrayBuffer.isView(value) && /^\d+$/.test(k)))
    .map(k => [k, __describe(value[k], ids)]);
  return out;
}`;

export const structuredCloneFamily = defineFamily({
  name: 'structured-clone',
  arbitrary: fc.record({
    values: fc.array(fc.record({ source: cloneValue, cycle: fc.boolean(), shared: fc.boolean() }), { minLength: 1, maxLength: 5 })
  }),
  build({ values }) {
    const body = values
      .map(({ source, cycle, shared }, i) => `{ const v = { root: ${source} };
${cycle ? 'v.self = v; if (v.root && typeof v.root === "object") v.root.back = v;' : ''}
${shared ? 'v.again = v.root;' : ''}
__recordName(${literal(`clone:${i}`)}, () => __describe(structuredClone(v)));
__recordName(${literal(`transfer:${i}`)}, () => { const b = new ArrayBuffer(2); const c = structuredClone({ v, b }, { transfer: [b] }); return [b.byteLength, c.b.byteLength, __describe(c.v)]; }); }`)
      .join('\n');
    return finish(`${describeSource}\n${body}`);
  }
});

const byteSegments = [
  [0x61], [0x7f], [0xc3, 0xa9], [0xe2, 0x82, 0xac], [0xf0, 0x9f, 0x98, 0x80], [0xef, 0xbb, 0xbf], [0x80], [0xbf], [0xc0, 0x80],
  [0xc2], [0xe2, 0x82], [0xf0, 0x9f, 0x98], [0xed, 0xa0, 0x80], [0xed, 0xbf, 0xbf], [0xf4, 0x90, 0x80, 0x80], [0xf8], [0xff], [0x00]
];
const bytes = fc.oneof(
  { weight: 3, arbitrary: fc.array(fc.constantFrom(...byteSegments), { maxLength: 8 }).map(segments => segments.flat()) },
  { weight: 1, arbitrary: fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 12 }) }
);

export const textcodec = defineFamily({
  name: 'textcodec',
  arbitrary: fc.record({
    inputs: fc.array(
      fc.record({
        bytes,
        splits: fc.uniqueArray(fc.integer({ min: 0, max: 12 }), { maxLength: 3 }).map(cuts => cuts.sort((a, b) => a - b)),
        text: text(6),
        room: fc.integer({ min: 0, max: 8 })
      }),
      { minLength: 1, maxLength: 5 }
    )
  }),
  build({ inputs }) {
    return finish(
      inputs
        .map(({ bytes, splits, text, room }, i) => `{ const u8 = new Uint8Array(${literal(bytes)});
__record(${literal(`decode:${i}`)}, () => new TextDecoder().decode(u8));
__record(${literal(`ignoreBOM:${i}`)}, () => new TextDecoder('utf-8', { ignoreBOM: true }).decode(u8));
__recordName(${literal(`fatal:${i}`)}, () => new TextDecoder('utf-8', { fatal: true }).decode(u8));
__record(${literal(`stream:${i}`)}, () => { const d = new TextDecoder(); const cuts = ${literal(splits)}.filter(c => c <= u8.length); let out = '', at = 0; for (const c of cuts) { out += d.decode(u8.subarray(at, c), { stream: true }); at = c; } return out + d.decode(u8.subarray(at)); });
__record(${literal(`utf16le:${i}`)}, () => new TextDecoder('utf-16le').decode(u8));
__record(${literal(`encode:${i}`)}, () => Array.from(new TextEncoder().encode(${literal(text)})));
__record(${literal(`encodeInto:${i}`)}, () => { const dst = new Uint8Array(${room}); const r = new TextEncoder().encodeInto(${literal(text)}, dst); return [r.read, r.written, Array.from(dst)]; });
__record(${literal(`buffer:${i}`)}, () => [Buffer.from(u8).toString('utf8'), Buffer.from(${literal(text)}).length]); }`)
        .join('\n')
    );
  }
});

const urlParts = [
  ['http:', 'https:', 'file:', 'ws:', 'wss:', 'blob:', 'data:', 'mailto:', 'javascript:', 'foo:', 'HTTP:', 'a+b.c-d:', ''],
  ['//', '/', '', '\\\\', '///', '/\\'],
  ['', 'user@', 'u:p@', ':@', '%40@', 'ä@'],
  [
    'example.com', 'EXAMPLE.com.', 'xn--nxasmq6b.com', 'ex%41mple.com', '127.0.0.1', '0x7f.1', '2130706433', '1.2.3.4.5', '[::1]',
    '[::ffff:1.2.3.4]', '[1:2::]', '[0:0::0]', 'bücher.de', 'a..b', '', 'local host', '%zz', '\u00ad.com', 'xn--', '256.0.0.1'
  ],
  ['', ':80', ':443', ':0', ':8080', ':65535', ':65536', ':abc', ':', ':021'],
  ['', '/', '/a/./b/../c', '/%2e%2E/x', '/a b', '/ä', '\\a\\b', '/;x', '//', '/..', '/./.', '/a%2fb', '/C:/x', '/|'],
  ['', '?', '?a=1&b=2', '?q=ä ö', '?%zz', "?'\"", '?a=1&a=2&=&x'],
  ['', '#', '#frag', '#ä x', '#%00']
];
// Ant vendors ada 4.0.0 and Node 22 ada 2.9.2, so some mismatches are
// version differences (opaque-path bases, xn-- labels); see tech-debt.md.
const urlInput = fc.tuple(...urlParts.map(values => fc.constantFrom(...values))).map(parts => parts.join(''));

export const url = defineFamily({
  name: 'url',
  arbitrary: fc.record({
    urls: fc.array(
      fc.record({
        input: urlInput,
        base: fc.constantFrom(null, 'http://base.example/dir/file?x#y', 'file:///C:/x/', 'about:blank', 'https://u:p@h:9/a/b')
      }),
      { minLength: 1, maxLength: 8 }
    ),
    queries: fc.array(
      fc.record({
        query: fc.tuple(text(6), fc.constantFrom('', '&', '=', '+', '%20', '%', '?x=1')).map(([a, b]) => a + b),
        name: text(3),
        value: text(3)
      }),
      { maxLength: 3 }
    )
  }),
  build({ urls, queries }) {
    const parsed = urls.map(({ input, base }, i) => {
      const args = base === null ? literal(input) : `${literal(input)}, ${literal(base)}`;
      return `__recordName(${literal(`url:${i}`)}, () => { const u = new URL(${args}); return [u.href, u.origin, u.protocol, u.username, u.password, u.host, u.hostname, u.port, u.pathname, u.search, u.hash]; });
__record(${literal(`canParse:${i}`)}, () => URL.canParse(${args}));`;
    });
    const searches = queries.map(({ query, name, value }, i) => `__record(${literal(`params:${i}`)}, () => {
  const p = new URLSearchParams(${literal(query)});
  const entries = [...p];
  p.append(${literal(name)}, ${literal(value)});
  const appended = p.toString();
  p.sort();
  return [entries, appended, p.toString(), p.get(${literal(name)}), p.has(${literal(name)})];
});`);
    return finish(parsed.concat(searches).join('\n'));
  }
});

const base64Alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_= \n.%'.split('');

export const bufferBase64 = defineFamily({
  name: 'buffer-base64',
  arbitrary: fc.record({
    inputs: fc.array(
      fc.record({
        bytes: fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 10 }),
        text: fc.array(fc.constantFrom(...base64Alphabet), { maxLength: 12 }).map(chars => chars.join('')),
        latin1: fc.array(fc.integer({ min: 0, max: 300 }), { maxLength: 6 }).map(codes => String.fromCharCode(...codes))
      }),
      { minLength: 1, maxLength: 5 }
    )
  }),
  build({ inputs }) {
    return finish(
      inputs
        .map(({ bytes, text, latin1 }, i) => `{ const b = Buffer.from(${literal(bytes)});
__record(${literal(`encode:${i}`)}, () => ['base64', 'base64url', 'hex', 'latin1', 'binary', 'ascii', 'ucs2'].map(e => b.toString(e)));
__record(${literal(`decode:${i}`)}, () => ['base64', 'base64url', 'hex', 'latin1', 'utf8', 'ucs2'].map(e => Array.from(Buffer.from(${literal(text)}, e))));
__record(${literal(`byteLength:${i}`)}, () => ['base64', 'base64url', 'hex', 'utf8', 'latin1'].map(e => Buffer.byteLength(${literal(text)}, e)));
__record(${literal(`roundtrip:${i}`)}, () => [Array.from(Buffer.from(b.toString('base64'), 'base64')), Array.from(Buffer.from(b.toString('base64url'), 'base64url'))]);
__recordName(${literal(`btoa:${i}`)}, () => btoa(${literal(latin1)}));
__recordName(${literal(`atob:${i}`)}, () => atob(${literal(text)}));
__recordName(${literal(`atob-btoa:${i}`)}, () => atob(btoa(String.fromCharCode(...b))) === String.fromCharCode(...b)); }`)
        .join('\n')
    );
  }
});
