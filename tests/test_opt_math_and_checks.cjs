const assert = require('node:assert');

const WARM = 200;

function describe(value) {
  if (Object.is(value, -0)) return '-0';
  if (typeof value === 'string') return JSON.stringify(value);
  if (Array.isArray(value)) return `[${value.map(describe).join(',')}]`;
  if (value && typeof value === 'object') return `{${Object.keys(value).map(k => `${k}:${describe(value[k])}`).join(',')}}`;
  return String(value);
}

const results = [];
function record(label, value) {
  results.push(`${label} ${describe(value)}`);
}

function attempt(f) {
  try {
    return f();
  } catch (e) {
    return `threw ${e.constructor.name}`;
  }
}

const EDGES = [
  NaN, 0, -0, Infinity, -Infinity, 5e-324, -5e-324, 2.2250738585072014e-308, -2.2250738585072014e-308,
  1e308, -1e308, 1.7976931348623157e308, 1, -1, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994,
  -0.49999999999999994, 0.5000000000000001, -0.5000000000000001, 3, -7, 2147483647, -2147483648, 2147483648,
  9007199254740992, 9007199254740994, -9007199254740992, 4503599627370495.5, -4503599627370495.5, 4503599627370497,
  1e-300, -1e-300, Math.PI, -Math.PI / 2, 1e21, 0.1, -0.9, 0.99, 1.0000000000000002, 0.9999999999999999, 100, -100, 710, -745,
];

const INTS = [0, 1, -1, 2, -2, 3, 7, -7, 100, -100, 2147483647, -2147483647, -2147483648, 65535, 12345];

const PAIRS = [];
for (const a of [NaN, 0, -0, Infinity, -Infinity, 1, -1, 2, -2, -8, 0.5, -0.5, 3, -3, 1e308, 5e-324, 1.5, -1.5, 10])
  for (const b of [NaN, 0, -0, Infinity, -Infinity, 1, -1, 2, -2, 0.5, -0.5, 1 / 3, 3, -3, 1e308, 5e-324, 1.5, 0.1, 1024, -1075])
    PAIRS.push([a, b]);

function unaryAll(xs) {
  const out = [];
  for (let i = 0; i < xs.length; i++) {
    const x = xs[i];
    out.push(Math.sin(x), Math.cos(x), Math.tan(x), Math.exp(x), Math.log(x), Math.atan(x), Math.asin(x), Math.acos(x));
    out.push(Math.sinh(x), Math.cosh(x), Math.tanh(x), Math.asinh(x), Math.acosh(x), Math.atanh(x), Math.expm1(x));
    out.push(Math.log1p(x), Math.log10(x), Math.log2(x), Math.cbrt(x));
    out.push(Math.round(x), Math.trunc(x), Math.floor(x), Math.ceil(x), Math.abs(x), Math.sqrt(x));
  }
  return out;
}

function intUnary(xs) {
  let s = 0;
  const out = [];
  for (let i = 0; i < xs.length; i++) {
    const x = xs[i] | 0;
    out.push(Math.round(x), Math.trunc(x), Math.abs(x), Math.sin(x), Math.pow(x, 2), x ** 3);
    s += Math.round(x) + Math.trunc(x);
  }
  out.push(s);
  return out;
}

function binaryAll(pairs) {
  const out = [];
  for (let i = 0; i < pairs.length; i++) {
    const p = pairs[i];
    out.push(Math.pow(p[0], p[1]), p[0] ** p[1], Math.atan2(p[0], p[1]));
  }
  return out;
}

function invariantPow(base, exp, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += Math.pow(base, exp) + Math.sin(base) * i;
  return s;
}

function mixedArgs(values) {
  const out = [];
  for (let i = 0; i < values.length; i++) out.push(attempt(() => Math.sin(values[i]) + Math.pow(values[i], 2)));
  return out;
}

function viaCallee(f, values) {
  const out = [];
  for (let i = 0; i < values.length; i++) out.push(f(values[i]));
  return out;
}

function exponent(values, e) {
  const out = [];
  for (let i = 0; i < values.length; i++) out.push(attempt(() => values[i] ** e));
  return out;
}

function mathCases() {
  let last;
  for (let k = 0; k < WARM; k++) last = unaryAll(EDGES);
  record('unary', last);
  for (let k = 0; k < WARM; k++) last = intUnary(INTS);
  record('int unary', last);
  for (let k = 0; k < WARM; k++) last = binaryAll(PAIRS);
  record('binary', last);
  for (let k = 0; k < WARM; k++) last = invariantPow(1.5, 2.5, 100);
  record('invariant pow', last);
  for (let k = 0; k < WARM; k++) last = mixedArgs([1, 2.5, -3]);
  record('mixed numbers', last);
  record('mixed strings', mixedArgs([1, '2', '0x10', 'abc', null, undefined, true, [3], { valueOf() { return 4; } }]));
  record('mixed bigint', mixedArgs([1, 2n]));
  for (let k = 0; k < WARM; k++) last = viaCallee(Math.sin, [0.5, -0, 2]);
  record('callee sin', last);
  record('callee cos', viaCallee(Math.cos, [0.5, -0, 2]));
  record('callee user', viaCallee(x => x * 2, [0.5, -0, 2]));
  for (let k = 0; k < WARM; k++) last = exponent([2, -2, 0.5, NaN, Infinity], 3);
  record('exponent', last);
  record('exponent nan', exponent([1, -1, NaN, 0], NaN));
  record('exponent inf', exponent([1, -1, 2, 0.5], Infinity));
  record('exponent bigint', exponent([2n, 3n], 2n));
  const saved = Math.pow;
  Math.pow = (a, b) => a * 1000 + b;
  record('patched pow', binaryAll([[2, 3], [4, 5]]));
  Math.pow = saved;
  record('restored pow', binaryAll([[2, 3], [4, 5]]));
}

function sumValues(rows) {
  let s = 0;
  for (let i = 0; i < rows.length; i++) s += rows[i].v;
  return s;
}

function sumLengths(items) {
  let s = 0;
  for (let i = 0; i < items.length; i++) s += items[i].length;
  return s;
}

function growWhileReading(rows) {
  let s = 0;
  for (let i = 0; i < rows.length; i++) {
    s += rows[i].v;
    if (rows[i].v === 3 && rows.length < 40) rows.push({ v: 1 });
    if (rows[i].v === 7) rows.pop();
  }
  return s + rows.length;
}

function lengthOf(x) {
  return attempt(() => x.length);
}

function codeUnits(str) {
  let s = 0;
  for (let i = 0; i < str.length; i++) s = (s * 31 + str.charCodeAt(i)) | 0;
  return s;
}

function codeUnitsPast(str, extra) {
  const out = [];
  for (let i = 0; i < str.length + extra; i++) out.push(str.charCodeAt(i));
  return out;
}

function readAt(rows, i) {
  return rows[i];
}

function readTwice(rows) {
  let s = 0;
  for (let i = 0; i < rows.length; i++) {
    if (rows[i].v > 2) s += rows[i].v;
    else s -= rows[i].w;
  }
  return s;
}

function nestedRows(grid) {
  let s = 0;
  for (let y = 0; y < grid.length; y++) {
    const row = grid[y];
    for (let x = 0; x < row.length; x++) s += row[x] * (y + 1);
  }
  return s;
}

function lengthAndElements(rows) {
  const out = [];
  for (let i = 0; i < rows.length; i++) out.push(describe(rows[i]));
  return out.join(';');
}

function arrayCases() {
  const rows = [];
  for (let i = 0; i < 30; i++) rows.push({ v: i % 9, w: 1 });
  let last;
  for (let k = 0; k < WARM; k++) last = sumValues(rows);
  record('sum values', last);
  const holey = rows.slice();
  holey[40] = { v: 100, w: 1 };
  record('sum values holey', attempt(() => sumValues(holey)));
  const sparse = rows.slice(0, 5);
  sparse.length = 8;
  record('sum values sparse', attempt(() => sumValues(sparse)));
  record('sum values typed', sumValues(new Float64Array([1, 2, 3])));
  record('sum values object', attempt(() => sumValues({ length: 2, 0: { v: 5 }, 1: { v: 6 } })));
  record('sum values string', attempt(() => sumValues('ab')));
  record('sum values null', attempt(() => sumValues(null)));

  const words = [];
  for (let i = 0; i < 40; i++) words.push('w' + i);
  for (let k = 0; k < WARM; k++) last = sumLengths(words);
  record('sum lengths', last);
  record('sum lengths mixed', sumLengths(['abc', [1, 2], 'héllo', 'a' + 'b'.repeat(40) + 'c', { length: 7 }, function (a, b) {}]));
  record('sum lengths numbers', attempt(() => sumLengths([1, 2, 3])));
  record('sum lengths holes', attempt(() => sumLengths(['a', , 'bc'])));

  for (let k = 0; k < WARM; k++) last = growWhileReading(rows.map((r) => ({ v: r.v })));
  record('grow while reading', last);

  for (let k = 0; k < WARM; k++) last = lengthOf([1, 2, 3]);
  record('length array', last);
  record('length string', lengthOf('héllo'));
  record('length rope', lengthOf('ab'.repeat(30) + 'é'));
  record('length object', lengthOf({ length: 'x' }));
  record('length getter', lengthOf({ get length() { return 42; } }));
  record('length function', lengthOf((a, b, c) => a));
  record('length number', lengthOf(5));
  record('length undefined', lengthOf(undefined));
  for (let k = 0; k < WARM; k++) last = lengthOf('abc');
  record('length string warm', last);
  record('length string array', lengthOf([1, 2]));
  record('length string non ascii', lengthOf('日本語'));

  let text = '';
  for (let i = 0; i < 200; i++) text += String.fromCharCode(97 + (i % 26));
  for (let k = 0; k < WARM; k++) last = codeUnits('hello world, plain ascii text');
  record('code units', last);
  record('code units rope', codeUnits(text));
  record('code units non ascii', codeUnits('ünïcödé ✓'));
  for (let k = 0; k < WARM; k++) last = codeUnitsPast('abcdef', 0);
  record('code units past', codeUnitsPast('abcdef', 2));

  for (let k = 0; k < WARM; k++) last = readAt(rows, k % 30);
  record('read at', last);
  record('read at past end', readAt(rows, 30));
  record('read at negative', readAt(rows, -1));
  record('read at fraction', readAt(rows, 1.5));
  record('read at string key', readAt(rows, 'length'));
  record('read at hole', readAt(holey, 35));
  Array.prototype[33] = 'from proto';
  record('read at proto', readAt(rows, 33));
  delete Array.prototype[33];
  record('read at proto removed', readAt(rows, 33));

  for (let k = 0; k < WARM; k++) last = readTwice(rows);
  record('read twice', last);
  const changed = rows.slice();
  changed[3] = { v: 'x', w: 2 };
  record('read twice changed', readTwice(changed));
  changed[4] = 5;
  record('read twice number', readTwice(changed));

  const grid = [];
  for (let y = 0; y < 12; y++) {
    const row = [];
    for (let x = 0; x < 12; x++) row.push(x + y * 0.5);
    grid.push(row);
  }
  for (let k = 0; k < WARM; k++) last = nestedRows(grid);
  record('nested rows', last);
  grid[3] = [1, 2];
  grid[5] = 'abc';
  record('nested rows changed', attempt(() => nestedRows(grid)));

  for (let k = 0; k < WARM; k++) last = lengthAndElements(['a', 'b', 'c']);
  record('length and elements', lengthAndElements(['a', 'b', 'c']));
  record('length and elements objects', lengthAndElements([{ x: 1 }, [2], null, undefined]));
}

function compareTwice(a, b) {
  let r = 0;
  if (a < b) {
    r += 1;
    if (a < b) r += 10;
    else r += 100;
  } else if (a < b) r += 1000;
  return r;
}

function compareAfterMerge(a, b, c) {
  let s = 0;
  if (a < b) s = c;
  if (a < b) s = (s + 1) | 0;
  return s;
}

function sameObject(x, y) {
  let r = 0;
  if (x === y) r += 1;
  if (x === y) r += 2;
  if (x !== y) r += 4;
  return r;
}

function lengthAfterPop(rows, i) {
  let r = 0;
  if (i < rows.length) {
    r += rows[i];
    rows.pop();
    if (i < rows.length) r += 100;
    else r += 1000;
  }
  return r;
}

function boundsTwice(rows, i) {
  let r = 0;
  if (i >= 0 && i < rows.length) r += rows[i];
  if (i >= 0 && i < rows.length) r += rows[i] * 2;
  return r;
}

function floatsTwice(a, b) {
  let r = 0;
  if (a <= b) r += 1;
  if (a <= b) r += 2;
  if (!(a <= b)) r += 4;
  return r;
}

function branchCases() {
  let last;
  for (let k = 0; k < WARM; k++) last = [compareTwice(k, 100), compareTwice(100, k)];
  record('compare twice', last);
  record('compare twice same', [compareTwice(3, 3), compareTwice(-1, -1), compareTwice('a', 'b'), compareTwice(1.5, 1.25)]);
  for (let k = 0; k < WARM; k++) last = [compareAfterMerge(k, 50, 7), compareAfterMerge(50, k, 7)];
  record('compare after merge', last);
  record('compare after merge strings', [compareAfterMerge('a', 'b', 3), compareAfterMerge(NaN, 1, 3)]);
  const o = { a: 1 };
  for (let k = 0; k < WARM; k++) last = [sameObject(o, o), sameObject(o, {}), sameObject(k, k)];
  record('same object', last);
  record('same object variants', [sameObject(NaN, NaN), sameObject(0, -0), sameObject('x', 'x'), sameObject(null, undefined)]);
  for (let k = 0; k < WARM; k++) last = lengthAfterPop([1, 2, 3, 4], k & 3);
  record('length after pop', [lengthAfterPop([1, 2, 3, 4], 3), lengthAfterPop([1, 2, 3, 4], 2), lengthAfterPop([1, 2, 3, 4], 4)]);
  for (let k = 0; k < WARM; k++) last = boundsTwice([5, 6, 7], k % 5 - 1);
  record('bounds twice', [boundsTwice([5, 6, 7], -1), boundsTwice([5, 6, 7], 2), boundsTwice([5, 6, 7], 3), boundsTwice([5, , 7], 1)]);
  for (let k = 0; k < WARM; k++) last = floatsTwice(k * 0.5, 50.25);
  record('floats twice', [floatsTwice(NaN, 1), floatsTwice(1, NaN), floatsTwice(-0, 0), floatsTwice(Infinity, Infinity)]);
}

function hoistField(o, other, n, at) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s = (s + o.x) | 0;
    if (i === at) s = (s + other.y) | 0;
  }
  return s;
}

function hoistMaybeNull(flag, obj, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (flag) s += obj.p;
    else s += 1;
  }
  return s;
}

function hoistAfterContinue(rows, k, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i < 2) continue;
    s += k[1][0] + rows.length;
  }
  return s;
}

function hoistCondition(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (o.kind === 1) s += o.a;
    else if (o.kind === 2) s += o.b * 2;
    else s -= 1;
  }
  return s;
}

function hoistCases() {
  let last;
  for (let k = 0; k < WARM; k++) last = hoistField({ x: k & 7 }, { y: 1 }, 20, 7);
  record('hoist field', last);
  record('hoist field other shape', hoistField({ x: 1 }, { z: 0, y: 2 }, 5, 3));
  record('hoist field never', hoistField({ x: 1 }, null, 5, 99));
  record('hoist field throws', attempt(() => hoistField({ x: 1 }, null, 5, 2)));
  for (let k = 0; k < WARM; k++) last = hoistField({ x: k & 7 }, { y: 1 }, 20, 7);
  record('hoist field again', last);
  for (let k = 0; k < WARM; k++) last = hoistMaybeNull(true, { p: 2 }, 10);
  record('hoist maybe null', [hoistMaybeNull(false, null, 10), hoistMaybeNull(false, undefined, 3), hoistMaybeNull(true, { q: 1, p: 3 }, 4)]);
  record('hoist maybe null throws', attempt(() => hoistMaybeNull(true, null, 3)));
  const k2 = [[1, 2], [3, 4]];
  for (let k = 0; k < WARM; k++) last = hoistAfterContinue([1, 2, 3], k2, 10);
  record('hoist after continue', last);
  record('hoist after continue short', [hoistAfterContinue([1], [[1], 'x'], 2), hoistAfterContinue('abc', [[1], [9]], 3)]);
  record('hoist after continue bad', attempt(() => hoistAfterContinue([1], [[1]], 4)));
  for (let k = 0; k < WARM; k++) last = hoistCondition({ kind: 1, a: 3, b: 4 }, 10);
  record('hoist condition', [last, hoistCondition({ kind: 2, a: 3, b: 4 }, 10), hoistCondition({ kind: 3 }, 10), hoistCondition({ b: 1, kind: 2 }, 3)]);
}

function readFirst(x, n) {
  let v;
  for (let i = 0; i < n; i++) v = x[0];
  return v;
}

function lengthLoop(x, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += x.length | 0;
  return s;
}

function fourthUnit(x, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += x.charCodeAt(3) | 0;
  return s;
}

function phiReads(n, rows) {
  let x = 0;
  let s = 0;
  for (let i = 0; i < n; i++) {
    const v = x[0];
    s += (v === undefined ? 1 : v) + (x.length | 0);
    x = i < n - 2 ? i : rows;
  }
  return s;
}

function guardCases() {
  let last;
  for (let k = 0; k < WARM; k++) last = phiReads(6, [k, 2]);
  record('guards phi reads', [last, phiReads(3, 'xyz'), phiReads(5, [7]), phiReads(4, null === 1 ? 0 : [])]);
  for (let k = 0; k < WARM; k++) {
    last = [readFirst([k], 4), readFirst({ 0: 'a' }, 4), readFirst('xy', 4)];
    last.push(lengthLoop([1, 2], 4), lengthLoop('abc', 4), lengthLoop({ length: 7 }, 4));
    last.push(fourthUnit('abcdef', 4), fourthUnit('ab', 4));
  }
  record('guards warm', last);
  const values = [5, 1.5, -0, NaN, 1e300, 5e-324, true, undefined, null, Symbol.iterator, 7n, 0x7ff8000000000, -1, {}, () => 1];
  record('guards read first', values.map((v) => attempt(() => readFirst(v, 4))));
  record('guards length loop', values.map((v) => attempt(() => lengthLoop(v, 4))));
  record('guards fourth unit', [new String('abcdef'), 'ab' + 'cd'.repeat(20), 'héllo', 'abc'].map((v) => attempt(() => fourthUnit(v, 4))));
}

function runCases() {
  mathCases();
  guardCases();
  arrayCases();
  branchCases();
  hoistCases();
  for (const line of results) console.log(line);
}

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const env = { ...process.env };
  delete env.ANT_DEBUG;
  delete env.FORCE_COLOR;
  const run = (debug, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...env, ANT_DEBUG: debug, NO_COLOR: '1' },
    timeout: 300000, maxBuffer: 64 * 1024 * 1024,
  });
  const interp = run('', ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['force', 'opt:force dump/vm:op-warn'],
    ['force verify', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  const expected = interp.stdout.split('\n');
  for (const [name, debug] of modes) {
    const child = run(debug);
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    const actual = child.stdout.split('\n');
    for (let i = 0; i < Math.max(actual.length, expected.length); i++)
      assert.strictEqual(actual[i], expected[i], `${name}: line ${i + 1} differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    console.log(`PASS opt math and checks ${name}`);
  }
}
