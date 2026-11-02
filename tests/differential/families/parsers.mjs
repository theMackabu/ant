import fc from 'fast-check';
import { finish, literal } from '../prelude.mjs';
import { stderrFailures } from './jit.mjs';
import { text } from './family.mjs';
import { jsSeeds, regexpSeeds } from './seeds.mjs';

const stringPieces = [
  'a', 'é', '😀', '\\x41', '\\x4', '\\x', '\\xG1', '\\u0041', '\\u004', '\\u', '\\u{41}', '\\u{1F600}', '\\u{10FFFF}',
  '\\u{110000}', '\\u{}', '\\u{00000041}', '\\uD83D', '\\uDE00', '\\uD83D\\uDE00', '\\0', '\\00', '\\08', '\\1', '\\7',
  '\\8', '\\9', '\\377', '\\400', '\\b', '\\f', '\\n', '\\r', '\\t', '\\v', '\\c', '\\q', '\\\\', "\\'", '\\"', '\\`',
  '\\$', '${', '${a}', '$', '}', '\\\n', '\\\r\n', '\\ ', ' ', ' ', '\n', '\r', '\r\n', "'", '"', '`'
];

const stringContexts = {
  single: body => "return '" + body + "'",
  double: body => 'return "' + body + '"',
  'strict-single': body => "'use strict'; return '" + body + "'",
  template: body => "const a = 'A'; return `" + body + '`',
  'strict-template': body => "'use strict'; const a = 'A'; return `" + body + '`',
  raw: body => "const a = 'A'; return String.raw`" + body + '`',
  tagged: body => "const a = 'A'; return ((s, ...v) => [Array.from(s), Array.from(s.raw), v])`" + body + '`',
  json: body => 'return JSON.parse(' + literal('"' + body + '"') + ')'
};

const identifierPieces = [
  'a', '_', '$', 'é', '℮', 'ℜ', '𝒜', '\\u0061', '\\u{62}', '\\u0024', '\\u005f', '\\u0030', '0', '\\u200c', '\\u200d',
  '\\u{1F600}', '\\u{10FFFF}', '\\u0069\\u0066', '\\u{6c}et', 'l\\u0065t', 'yi\\u0065ld', 'aw\\u0061it', '\\u0061sync',
  '\\uD835\\uDC9C', '\\u{1D49C}', '\\u'
];

const identifierContexts = {
  var: id => 'var ' + id + ' = 1; return ' + id,
  'strict-let': id => "'use strict'; let " + id + ' = 1; return ' + id,
  property: id => 'return Object.keys({ ' + id + ': 1 })',
  member: id => 'const o = { ' + id + ': 2 }; return o.' + id,
  private: id => 'class C { #' + id + ' = 3; static get(o) { return o.#' + id + '; } } return C.get(new C())',
  label: id => id + ': for (;;) break ' + id + '; return 4',
  function: id => 'function ' + id + '() { return 5; } return ' + id + '()'
};

const pieceCase = (pieces, contexts) => fc.record({
  pieces: fc.array(fc.constantFrom(...pieces), { minLength: 1, maxLength: 6 }),
  context: fc.constantFrom(...Object.keys(contexts))
});

export const escapes = {
  name: 'escapes',
  arbitrary: fc.record({
    strings: fc.array(pieceCase(stringPieces, stringContexts), { minLength: 1, maxLength: 8 }),
    identifiers: fc.array(pieceCase(identifierPieces, identifierContexts), { maxLength: 4 })
  }),
  build({ strings, identifiers }) {
    const record = (kind, contexts) => ({ pieces, context }, i) => {
      const source = contexts[context](pieces.join(''));
      return `__recordName(${literal(`${kind}:${i}:${context}`)}, () => new Function(${literal(source)})());`;
    };
    return finish([...strings.map(record('string', stringContexts)), ...identifiers.map(record('identifier', identifierContexts))].join('\n'));
  }
};

const rarely = (valid, invalid) => fc.oneof({ weight: 12, arbitrary: fc.constantFrom(...valid) }, { weight: 1, arbitrary: fc.constantFrom(...invalid) });

const ws = rarely(['', '', '', ' ', '\t', '\n', '\r'], [' ', '﻿', ' ', '\v']);
const jsonNumber = rarely(
  ['0', '-0', '1', '-1', '1.5', '-1.5e-7', '1e5', '1E+5', '1e-5', '1e400', '-1e400', '1e-400', '123456789012345678901234567890', '9007199254740993', '2.2250738585072011e-308', '4.9e-324', '0.1e-999', '-0.0'],
  ['01', '-01', '-', '1.', '.5', '1e', '1e+', '+1', '0x1', 'Infinity', 'NaN', '1_0', '00']
);
const jsonStringPiece = rarely(
  ['a', 'é', '😀', '\\"', '\\\\', '\\/', '\\b', '\\f', '\\n', '\\r', '\\t', '\\u0041', '\\u00e9', '\\uD83D\\uDE00', '\\uD800', '\\uDFFF', ' ', '\u007f', ' ', '\\u0000'],
  ['\\u12', '\\u12G4', '\\x41', '\\a', "\\'", '\u0001', '\u001f', '\t']
);
const jsonLiteral = rarely(['true', 'false', 'null'], ['tru', 'nul', 'True', 'undefined', '"']);
const jsonString = fc.array(jsonStringPiece, { maxLength: 4 }).map(pieces => '"' + pieces.join('') + '"');
const jsonKey = fc.oneof(rarely(['"a"', '"b"', '""', '"__proto__"', '"constructor"', '"0"', '"1"', '"-1"', '"01"', '"4294967295"', '"4294967294"', '"a\\u0000b"'], ["'a'", 'a']), jsonString);
const colon = rarely([':'], ['', '::']);
const trailing = rarely([''], [',']);

const { jsonValueText } = fc.letrec(tie => ({
  jsonValueText: fc.oneof(
    { maxDepth: 3, depthIdentifier: 'json-text' },
    jsonNumber,
    jsonString,
    jsonLiteral,
    fc.tuple(fc.array(fc.tuple(ws, tie('jsonValueText'), ws), { maxLength: 4 }), trailing)
      .map(([items, comma]) => '[' + items.map(([a, v, b]) => a + v + b).join(',') + comma + ']'),
    fc.tuple(fc.array(fc.tuple(jsonKey, ws, colon, tie('jsonValueText')), { maxLength: 4 }), trailing)
      .map(([entries, comma]) => '{' + entries.map(([k, a, c, v]) => k + a + c + v).join(',') + comma + '}')
  )
}));

const orderedSource = String.raw`
function __ordered(value) {
  if (typeof value !== 'object' || value === null) return __canon(value);
  if (Array.isArray(value)) return ['$array', value.length].concat(value.map(__ordered));
  return ['$object', Object.getPrototypeOf(value) === Object.prototype].concat(Object.keys(value).map(key => [key, __ordered(value[key])]));
}
function __depth(value) {
  let depth = 0;
  while (value !== null && typeof value === 'object') { value = Array.isArray(value) ? value[0] : value.a; depth++; }
  return [depth, __canon(value)];
}`;

export const jsonText = {
  name: 'json-text',
  arbitrary: fc.record({
    texts: fc.array(
      fc.record({
        before: ws,
        value: jsonValueText,
        after: ws,
        nesting: fc.constantFrom(0, 0, 0, 0, 1, 64, 1000, 5000),
        wrapper: fc.constantFrom('array', 'object')
      }),
      { minLength: 1, maxLength: 5 }
    )
  }),
  build({ texts }) {
    const body = texts.map(({ before, value, after, nesting, wrapper }, i) => {
      const open = wrapper === 'array' ? '[' : '{"a":';
      const close = wrapper === 'array' ? ']' : '}';
      const source = open.repeat(nesting) + before + value + after + close.repeat(nesting);
      const t = literal(source);
      if (nesting > 64) return `__recordName(${literal(`deep:${i}`)}, () => __depth(JSON.parse(${t})));`;
      return `__recordName(${literal(`parse:${i}`)}, () => __ordered(JSON.parse(${t})));
__recordName(${literal(`stringify:${i}`)}, () => JSON.stringify(JSON.parse(${t})));
__recordName(${literal(`reviver:${i}`)}, () => { const log = []; JSON.parse(${t}, function (key, value) { log.push([key, typeof value, Array.isArray(this)]); return value; }); return log; });
__recordName(${literal(`prune:${i}`)}, () => __ordered(JSON.parse(${t}, (key, value) => key.length === 1 ? undefined : value)));`;
    });
    return finish(`${orderedSource}\n${body.join('\n')}`);
  }
};

const regexpAtoms = [
  'a', 'b', 'A', '.', '\\d', '\\D', '\\w', '\\W', '\\s', '\\S', '\\b', '\\B', '^', '$', '\\n', '\\t', '\\0', '\\x41',
  '\\u0041', '\\u{1F600}', '\\uD83D', '\\uDE00', '\\uD83D\\uDE00', '😀', '\\p{L}', '\\P{Lu}', '\\p{Script=Greek}',
  '\\p{RGI_Emoji}', '\\cJ', '\\c', '\\/', '\\-', '-', ']', '}', '{', '\\k<n>', '\\1', '\\2', '\\10', 'ß', 'K', 'K',
  'ſ', 'İ', 'ı', 'é', '\\a', '\\8'
];
const classItems = ['a', 'b', 'z', 'a-z', 'A-Z', '0-9', '\\d', '\\w', '\\s', '\\b', '\\-', '-', '^', '😀', '\\u{1F600}', '\\uD83D', 'é', 'K', 'K', 'ſ', ']', '[', '\\p{L}', '\\q{ab|c}', '[a-c]', '&&', '--'];
const groupOpeners = ['(', '(?:', '(?<n>', '(?<m>', '(?=', '(?!', '(?<=', '(?<!', '(?i:', '(?-i:', '(?ms-i:', '(?i-i:'];
const quantifiers = ['', '', '', '', '*', '+', '?', '{2}', '{1,3}', '{0,}', '{2,1}', '{,2}', '*?', '+?', '??', '{1,2}?', '{'];
const regexpFlags = ['', '', 'g', 'i', 'm', 's', 'u', 'v', 'y', 'd', 'gi', 'giu', 'gv', 'iv', 'imsu', 'dgu', 'gy', 'uv', 'gg', 'x'];
const subjects = ['', 'a', 'ab', 'aAbB', 'abc abc', 'A\nb\nc', '😀', 'a😀b', '\uD83D', '\uDE00a', 'KKk', 'ßSS', 'ſs', 'İi', 'aaaaaaaaaa', 'é́', '0123', ' \t\n'];
const quantifier = fc.constantFrom(...quantifiers);
const seedSubjects = ['2026-11-01 2026-11-01', 'a\nb word wordy', 'K\u212aſ😀ΣΑΣ', 'aab abc'];

const { pattern } = fc.letrec(tie => ({
  pattern: fc.oneof(
    { maxDepth: 3, depthIdentifier: 'regexp-gen' },
    fc.tuple(fc.constantFrom(...regexpAtoms), quantifier).map(([atom, q]) => atom + q),
    fc.tuple(fc.boolean(), fc.array(fc.constantFrom(...classItems), { maxLength: 4 }), quantifier).map(([negated, items, q]) => '[' + (negated ? '^' : '') + items.join('') + ']' + q),
    fc.tuple(fc.constantFrom(...groupOpeners), tie('pattern'), quantifier).map(([open, inner, q]) => open + inner + ')' + q),
    fc.array(tie('pattern'), { minLength: 2, maxLength: 3 }).map(parts => parts.join('')),
    fc.tuple(tie('pattern'), tie('pattern')).map(([left, right]) => left + '|' + right)
  )
}));

const matchSource = String.raw`
function __match(m) {
  if (m === null) return null;
  const out = [m.index, Array.from(m, __canon)];
  out.push(m.groups === undefined ? '$none' : Object.entries(m.groups).map(([k, v]) => [k, __canon(v)]));
  if (m.indices) {
    out.push(Array.from(m.indices, pair => pair ? Array.from(pair) : '$none'));
    out.push(m.indices.groups === undefined ? '$none' : Object.entries(m.indices.groups).map(([k, v]) => [k, v ? Array.from(v) : '$none']));
  }
  return out;
}
function __regexpCase(label, source, flags, inputs) {
  let r;
  __recordName('new:' + label, () => { r = new RegExp(source, flags); return [r.source, r.flags, r.lastIndex]; });
  if (!r) return;
  inputs.forEach((s, j) => {
    const at = label + ':' + j;
    __recordName('exec:' + at, () => { r.lastIndex = 0; const first = __match(r.exec(s)); const firstIndex = r.lastIndex; return [first, firstIndex, __match(r.exec(s)), r.lastIndex]; });
    __recordName('all:' + at, () => { r.lastIndex = 0; return r.global ? Array.from(s.matchAll(r), m => [m.index, m[0]]) : __match(s.match(r)); });
    __recordName('replace:' + at, () => { r.lastIndex = 0; return [s.replace(r, '[$&|$1|$<n>|$\x60|$\'|$$]'), s.replace(r, (...args) => '{' + args.length + '}')]; });
    __recordName('replaceAll:' + at, () => { r.lastIndex = 0; return r.global ? s.replaceAll(r, '<$&>') : '$skip'; });
    __recordName('split:' + at, () => s.split(r, 6));
    __recordName('search:' + at, () => { r.lastIndex = 0; return [s.search(r), r.test(s), r.lastIndex]; });
  });
}`;

export const regexpGen = {
  name: 'regexp-gen',
  arbitrary: fc.record({
    patterns: fc.array(
      fc.record({
        source: pattern,
        flags: fc.constantFrom(...regexpFlags),
        inputs: fc.array(fc.oneof(fc.constantFrom(...subjects), text(6)), { minLength: 1, maxLength: 3 })
      }),
      { minLength: 1, maxLength: 4 }
    )
  }),
  build({ patterns }) {
    const body = patterns.map(({ source, flags, inputs }, i) => `__regexpCase(${literal(String(i))}, ${literal(source)}, ${literal(flags)}, ${literal(inputs)});`);
    for (const seed of regexpSeeds) body.push(`__regexpCase(${literal(`seed:${seed.name}`)}, ${literal(seed.source)}, ${literal(seed.flags)}, ${literal(seedSubjects)});`);
    const escaped = patterns.flatMap(({ inputs }) => inputs).slice(0, 4);
    return finish(`${matchSource}\n${body.join('\n')}\n__recordName('escape', () => ${literal(escaped)}.map(s => RegExp.escape(s)));`);
  }
};

const syntaxFragments = [
  'let x = 1', 'let [a, ...b] = [1, 2]', 'const { c = 1, ...r } = {}', 'var let = 1', 'let\nlet', 'yield', 'await', 'await 1',
  'async function f() { await 1 }', 'function* g() { yield* [] }', 'async () => {}', '(a, b) => a', '(a, b)\n=> a', 'x\n++y',
  'a = b\n(c)', 'return', 'return 1', 'a?.b', 'a?.[0]', 'a?.()', 'a?.b`t`', 'new.target', 'import.meta', 'super.x', 'super()',
  'class A { #x = 1; static { this.y = 1 } get #z() { return 1 } m() { return this.#x } }', 'class B extends Object { constructor() { super() } }',
  '#x in o', 'label: for (;;) break label', 'break', 'continue', 'if (a) function f() {}', 'with (o) {}', '"use strict"',
  "'use strict'", '0777', '08', '1_000', '1__0', '0x_1', '1n', '01n', '.5e1', '5..toString()', '/re/g', 'a / b / c', 'x = /=/',
  '`${`${a}`}`', '<!-- html', '--> html', 'for (let x of [])', 'for (let in o)', 'for (async of [])', 'for await (x of y)',
  'using u = null', 'await using v = null', 'try {} catch {} finally {}', 'switch (a) { case 1: default: }', 'do x; while (0) y',
  'delete x', 'typeof await', 'a ?? b || c', '(a ?? b) || c', 'a ** -b', '(-a) ** b', '[a, b] = [b, a]', '({ a } = b)',
  '({ a }) = b', 'async\nfunction h() {}', 'get', 'set', 'arguments = 1', 'eval = 1', 'function k(a, a) {}', 'function k(a = 1, a) {}',
  '"\\08"', '`\\08`', 'tag`\\08`', 'a\n?.5:1', 'a ? b : c', 'x => {}', '{ let x; { var x } }', 'let x; let x', 'var x; var x',
  'function f() { "use strict"; 010 }', '({ __proto__: 1, __proto__: 2 })', '({ get a() {}, get a() {} })', 'new.target = 1',
  '(async function* () { yield await 1 })', 'class C { static async *#m() {} }', 'x = { a, b, ...c }', 'f(...a, ...b,)', 'f(,)',
  '[,,]', '({ a: 1, })', 'a = 1, b = 2', '{}', ';', '', 'if (a) else b', 'import("x")', 'import x from "y"', 'export default 1'
];
const syntaxSeparators = [';\n', ';\n', ';\n', ';', '\n', ' '];
const syntaxEdits = ['(', ')', '{', '}', '[', ']', ';', ',', '=', '/', '`', '"', "'", '\n', '\\', '?', '.', '#', '@', '=>', '...', '*'];

const syntaxModes = String.raw`
const __vm = require('node:vm');
globalThis.__syntaxStop = Symbol('stop');
const __modes = [
  ['script', s => new __vm.Script(s)],
  ['strict', s => new __vm.Script('"use strict";\n' + s)],
  ['eval', s => { try { (0, eval)('throw globalThis.__syntaxStop;\n' + s); } catch (error) { if (error !== globalThis.__syntaxStop) throw error; } }],
  ['function', s => new Function(s)],
  ['async', s => new (async function () {}).constructor(s)],
  ['generator', s => new (function* () {}).constructor(s)]
];
async function __syntaxCase(label, source) {
  const out = __modes.map(([name, make]) => { try { make(source); return name; } catch (error) { return name + ':' + (error && error.name); } });
  out.push(await import('data:text/javascript,' + encodeURIComponent('throw 0;\n' + source)).then(() => 'module', error => error && error.name === 'SyntaxError' ? 'module:SyntaxError' : 'module'));
  __out.push([label, __canon(out)]);
}`;

export const syntax = {
  name: 'syntax',
  variants: [{ name: 'default', env: { ANT_DEBUG: 'dump/vm:op-warn' }, stderrFailures }],
  arbitrary: fc.record({
    programs: fc.array(
      fc.record({
        fragments: fc.array(fc.tuple(fc.constantFrom(...syntaxFragments), fc.constantFrom(...syntaxSeparators)), { minLength: 1, maxLength: 3 }),
        edits: fc.oneof({ weight: 3, arbitrary: fc.constant([]) }, { weight: 1, arbitrary: fc.array(fc.tuple(fc.constantFrom(...syntaxEdits), fc.double({ min: 0, max: 1, noNaN: true }), fc.integer({ min: 0, max: 2 })), { minLength: 1, maxLength: 2 }) })
      }),
      { minLength: 1, maxLength: 8 }
    )
  }),
  build({ programs }) {
    const body = programs.map(({ fragments, edits }, i) => {
      let source = fragments.map(([fragment, separator]) => fragment + separator).join('');
      for (const [edit, at, remove] of edits) {
        const position = Math.floor(source.length * at);
        source = source.slice(0, position) + edit + source.slice(position + remove);
      }
      return `await __syntaxCase(${literal(`program:${i}`)}, ${literal(source)});`;
    });
    for (const seed of jsSeeds) body.push(`await __syntaxCase(${literal(`seed:${seed.name}`)}, ${literal(seed.source)});`);
    return finish(`${syntaxModes}\n${body.join('\n')}`, true);
  }
};
