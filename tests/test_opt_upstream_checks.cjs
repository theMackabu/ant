const assert = require('node:assert');

const warm = 300;

function describe(value) {
  if (Object.is(value, -0)) return '-0';
  if (typeof value === 'bigint') return `${value}n`;
  if (typeof value === 'string') return JSON.stringify(value);
  if (typeof value === 'symbol') return value.toString();
  if (Array.isArray(value)) return `[${value.map(describe).join(',')}]`;
  return String(value);
}

function show(label, values, expected) {
  const text = values.map(describe).join(' ');
  console.log(label, text);
  if (expected !== undefined) assert.strictEqual(text, expected.map(describe).join(' '), label);
}

function attempt(f) {
  try {
    return f();
  } catch (e) {
    return `threw ${e.constructor.name}`;
  }
}

function plusZero(a, b) {
  const m = (a + b) / -2;
  return m + 0;
}

function orZero(a, b) {
  const m = (a + b) / 2;
  return m | 0;
}

function orZeroLocal(a, b) {
  let m = (a + b) / 2;
  m = m - 0;
  return m | 0;
}

function shifts(a, b) {
  const m = (a + b) / 2;
  return (m << 0) + (m >> 0) + (m ^ 0);
}

function midpoint(low, high) {
  return ((low + high) / 2) | 0;
}

function checkedSum(a, b, count, start) {
  let y = a * b;
  let s = start;
  for (let i = 0; i < count; i++) {
    let t = y;
    if (i > 1000) t = y;
    y = t;
    s = s + y;
  }
  return s;
}

function withDefault(a, b = a + 1, ...rest) {
  return a + b + rest.length;
}

function withDefaults(a, b = a * 2, c = b + 1, ...rest) {
  let s = a + b + c;
  for (const r of rest) s += r;
  return s;
}

function nothing() {}

function callAndName(target) {
  try {
    return target();
  } catch (e) {
    return e.constructor.name;
  }
}

const arrowCapture = () => {
  let body = '';
  for (let i = 0; i < 1000; ++i) body += `r${i + 0.5};`;
  const missing = nothing(body);
  return callAndName(() => missing());
};

const arrowValue = () => {
  let total = 0;
  for (let i = 0; i < 1000; ++i) total += i;
  const captured = total;
  return [() => captured + 1, () => captured * 2].map((f) => f());
};

function collect(iterable) {
  const out = [];
  for (const value of iterable) out.push(value);
  return out;
}

function shortenedArguments() {
  'use strict';
  arguments.length = 1;
  return arguments;
}

function lengthenedArguments() {
  arguments.length = 4;
  return arguments;
}

function putKeyed(object, key, value) {
  object[key] = value;
}

function putKeyedStrict(object, key, value) {
  'use strict';
  object[key] = value;
}

function remainder(a, b) {
  return a % b;
}

function remainderOfFive(b) {
  return 5 % b;
}

function viaClosure(f) {
  try {
    return f();
  } catch (e) {
    return `threw ${e.constructor.name}`;
  }
}

function tryAfterReturn(a) {
  try {
    if (a > 1000) return 'early';
    undefined.x;
  } catch (e) {
    return 'caught';
  }
  return 'none';
}

function tryAfterForOf(a, b) {
  try {
    for (const e of [a, b]) typeof e;
    undefined.x;
  } catch (e) {
    return 'caught';
  }
  return 'none';
}

function tryAfterBreak(a) {
  try {
    for (const x of [1, 2]) {
      if (x === a) break;
    }
    undefined.x;
  } catch (e) {
    return 'caught';
  }
  return 'none';
}

function tryNested(a) {
  try {
    try {
      if (a & 1) throw 1;
    } catch (e) {
      a = -a;
    }
    undefined.x;
  } catch (e) {
    return a;
  }
  return 'none';
}

function tryInLoop(a) {
  let r = 0;
  for (let i = 0; i < 3; i++) {
    try {
      if (i === a % 3) continue;
      r += i;
      null.y;
    } catch (e) {
      r += 10;
    }
  }
  return r;
}

function tryAfterLoopReturn(a) {
  for (const x of [a, a + 1]) {
    try {
      if (x & 1) return x;
    } catch (e) {}
  }
  try {
    undefined.q;
  } catch (e) {
    return 'caught';
  }
}

function loopInTry(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; ++i) s += i;
  } catch (e) {
    return 'inner caught';
  }
  return s + missingGlobal;
}

function loopInTryReturning(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; ++i) s += i;
  } catch (e) {
    return 'inner caught';
  }
  return s;
}

function afterLoopInTry() {
  const r = loopInTryReturning(2000);
  return r + missingGlobal;
}

function negatedSum(a, b) {
  return -a + b;
}

function negatedProduct(a, b) {
  return (-a) * b - 1;
}

function negatedLength(p) {
  return (-(p - p)) + p.length;
}

const resumeLog = [];

function resumeInTry(a, b) {
  try {
    for (const element of [b, 'abc']) resumeLog.push(typeof element);
    if ([((a / a) ? (notDefined >>> 1) : Math.imul(a, b)), ((-2147483648) / a) ?? (~b)]) throw new RangeError('thrown ' + a);
  } catch (e) {
    return e.constructor.name;
  }
}

function bigIntEquals(a, b) {
  return a == b;
}

function negatedLoop(a, n) {
  let s = -a;
  for (let i = 0; i < n; i++) s = s * 1.5 + 0.25;
  return s;
}

function negatedMax(a, b) {
  return Math.max(-a, b) + 1;
}

function negatedPhi(a, c) {
  const s = c ? -a : a * 2;
  return s - 1;
}

function deleteNamed(object) {
  return delete object.x;
}

function deleteKeyed(object, key) {
  return delete object[key];
}

function runCases() {
  const captures = [];
  for (let k = 0; k < 3; k++) captures.push(arrowCapture(), ...arrowValue());
  show('arrow functions capturing bindings of arrow functions', captures,
    ['TypeError', 499501, 999000, 'TypeError', 499501, 999000, 'TypeError', 499501, 999000]);

  for (let k = 0; k < warm; k++) {
    plusZero(k, k + 2);
    orZero(k, k + 2);
    orZeroLocal(k, k + 2);
    shifts(k, k + 2);
    midpoint(k, k + 2);
  }
  show('int32 identities of checked doubles', [
    plusZero(0, 0), plusZero(1, 2), orZero(1, 2), orZero(-1, -2), orZeroLocal(1, 2), orZeroLocal(-1, -2),
    orZeroLocal(0, NaN), shifts(1, 2), shifts(2 ** 31, 2 ** 31 + 3), midpoint(1, 2), midpoint(-1, -2), midpoint(0, NaN),
    midpoint(2 ** 32, 2 ** 32 + 6),
  ], [0, -1.5, 1, -1, 1, -1, 0, 3, -6442450941, 1, -1, 0, 3]);

  for (let k = 0; k < warm; k++) {
    checkedSum(1.5, 2, 3, 0);
    checkedSum(1.5, 2, 2, 'a');
  }
  show('loop values of checked doubles', [
    checkedSum(1.5, 2, 3, 0), checkedSum(1.5, 2, 2, 'a'), checkedSum(1.25, 2, 2, 1), checkedSum(1.25, 2, 2, 'b'),
  ], [9, 'a33', 6, 'b2.52.5']);

  let total = 0;
  for (let k = 0; k < warm; k++) total += withDefault(k) + withDefault(k, 1, 2, 3) + withDefaults(k) + withDefaults(k, 1, 2, 3, 4);
  show('default and rest parameters', [
    total, withDefault(1), withDefault(1, undefined, 5), withDefault(1, 2, 3, 4), withDefaults(1), withDefaults(1, 2, 3, 4),
  ], [408150, 3, 4, 5, 6, 10]);

  const shortened = shortenedArguments(1, 2, 3);
  const lengthened = lengthenedArguments(5, 6);
  const iterated = [];
  for (let k = 0; k < warm; k++) {
    collect([1, 2, 3]);
    if (k % 50 === 0) iterated.push(collect(shortened).join(), collect(lengthened).join());
  }
  show('for-of over arguments objects with changed lengths', [...new Set(iterated), collect(shortened).length, collect(lengthened).length],
    ['1', '5,6,,', 1, 4]);

  for (let k = 0; k < warm; k++) {
    putKeyed({}, 'k' + (k & 7), k);
    putKeyedStrict({}, 'k' + (k & 7), k);
  }
  const sym = Symbol('key');
  const keys = [
    { toString() {} },
    { toString() { throw new RangeError('key'); } },
    { [Symbol.toPrimitive]() { return sym; } },
    Symbol.iterator,
  ];
  const puts = [];
  for (const key of keys) {
    for (const receiver of [() => ({}), () => [0, 1], () => 's1xyz', () => 42, () => true, () => Symbol('r'), () => 5n]) {
      for (const put of [putKeyed, putKeyedStrict]) {
        const object = receiver();
        const result = attempt(() => put(object, key, 9));
        puts.push(typeof result === 'string' ? result : typeof object === 'object' ? Reflect.ownKeys(object).length : 'ok');
      }
    }
  }
  const primitivePuts = [1, 1, 4, 4, ...new Array(5).fill(['ok', 'threw TypeError']).flat()];
  show('keyed puts with converted and symbol keys', puts,
    [...primitivePuts, ...new Array(14).fill('threw RangeError'), ...primitivePuts, ...primitivePuts]);
  for (let k = 0; k < warm; k++) {
    deleteNamed({ x: k });
    deleteKeyed({ x: k }, 'x');
  }
  const deletes = [];
  for (const object of ['s1', 42, true, Symbol('r'), 5n]) {
    deletes.push(deleteNamed(object), deleteKeyed(object, 0), deleteKeyed(object, 'length'), deleteKeyed(object, Symbol.iterator),
      deleteKeyed(object, 5));
  }
  show('deletes of properties of primitives', deletes, [true, false, false, true, true, ...new Array(20).fill(true)]);

  const setterTag = Symbol('setter');
  const seen = [];
  Object.defineProperty(Number.prototype, setterTag, { set(v) { seen.push(typeof this, v); }, configurable: true });
  for (let k = 0; k < 3; k++) putKeyed(7, setterTag, k);
  delete Number.prototype[setterTag];
  show('symbol setters on primitive prototypes', seen, ['object', 0, 'object', 1, 'object', 2]);

  const tries = [];
  const tryFunctions = [tryAfterReturn, tryAfterForOf, tryAfterBreak, tryNested, tryInLoop, tryAfterLoopReturn];
  for (let k = 0; k < warm; k++) {
    for (const f of tryFunctions) {
      let r;
      try {
        r = f(k, k);
      } catch (e) {
        r = `escaped ${f.name}`;
      }
      if (k >= warm - 2) tries.push(r);
    }
  }
  show('exceptions in try regions of compiled code', tries,
    ['caught', 'caught', 'caught', 298, 22, 299, 'caught', 'caught', 'caught', -299, 21, 299]);

  for (let k = 0; k < warm; k++) resumeInTry(5, k, k);
  const resumed = [];
  for (const a of [0, Infinity, -0, NaN, 5]) {
    try {
      resumed.push(resumeInTry(a, 1, 1));
    } catch (e) {
      resumed.push(`escaped ${e.message}`);
    }
  }
  show('exits to the interpreter inside try regions', resumed, ['RangeError', 'RangeError', 'RangeError', 'RangeError', 'ReferenceError']);

  let equalCount = 0;
  for (let k = 0; k < 20000; k++) {
    if (bigIntEquals(1n, -0)) equalCount++;
    if (bigIntEquals(k & 1 ? 0n : 3n, k & 1 ? -0 : 3)) equalCount++;
    const garbage = { k };
    if (garbage.k < 0) equalCount--;
  }
  show('loose equality of BigInts and numbers', [equalCount, bigIntEquals(0n, -0), bigIntEquals(-5n, -5), bigIntEquals(2n ** 60n, 2 ** 60),
    bigIntEquals(1n, 1.5)], [20000, true, true, true, false]);

  const osrTries = [];
  for (let k = 0; k < 3; k++) {
    osrTries.push(attempt(() => loopInTry(2000)), attempt(afterLoopInTry), loopInTryReturning(10));
  }
  show('on-stack replacement inside try regions', osrTries,
    ['threw ReferenceError', 'threw ReferenceError', 45, 'threw ReferenceError', 'threw ReferenceError', 45,
      'threw ReferenceError', 'threw ReferenceError', 45]);

  const arrays = [[1, 2, 3, 4], [5, 6, 7, 8]];
  let lengths = 0;
  for (let k = 0; k < warm; k++) {
    negatedSum(k + 0.5, 1.5);
    negatedProduct(k + 0.5, 2.5);
    lengths = negatedLength(arrays[k & 1]);
  }
  for (let k = 0; k < warm; k++) {
    negatedLoop(k + 0.5, 3);
    negatedMax(k + 0.5, 1.5);
    negatedPhi(k + 0.5, k & 1);
  }
  show('arithmetic on negated NaNs', [negatedSum(NaN, 1), negatedProduct(NaN, 2), negatedSum(1, NaN), negatedProduct(0 / 0, 0.5),
    lengths, negatedLength([1]), typeof negatedSum(NaN, 0), negatedLoop(NaN, 3), negatedLoop(NaN, 0), negatedMax(NaN, 1),
    negatedPhi(NaN, 1), negatedPhi(NaN, 0)], [NaN, NaN, NaN, NaN, NaN, 1, 'number', NaN, NaN, NaN, NaN, NaN]);

  const remainders = [];
  for (let k = 0; k < warm; k++) {
    viaClosure(() => 0);
    remainders.length = 0;
    remainders.push(viaClosure(() => remainderOfFive((k & 7) - 3)));
  }
  const operands = [5, -5, 0, -0, 3, -3, 0.5, 2.5, Infinity, -Infinity, NaN, 2 ** 53, 2 ** 60, 1e300, 7];
  for (let k = 0; k < warm; k++) {
    viaClosure(() => 0);
    viaClosure(() => remainder(k & 7, (k & 3) + 1));
  }
  const inlined = [];
  for (const a of operands) for (const b of operands) {
    viaClosure(() => 0);
    inlined.push(viaClosure(() => remainder(a, b)));
  }
  const expected = [];
  for (const a of operands) for (const b of operands) expected.push(a % b);
  show('inlined remainders', [viaClosure(() => remainderOfFive(0)), viaClosure(() => remainderOfFive(-0)), ...inlined],
    [NaN, NaN, ...expected]);
}

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  delete base.FORCE_COLOR;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const reference = run({}, ['--jitless']);
  assert.strictEqual(reference.status, 0, reference.stdout + reference.stderr);

  const modes = [
    ['default', {}],
    ['verify', { ANT_DEBUG: 'opt:verify' }],
    ['force', { ANT_DEBUG: 'opt:force,verify' }],
    ['off', { ANT_DEBUG: 'opt:off' }],
  ];
  for (const [name, env] of modes) {
    const child = run(env);
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, reference.stdout, `${name}: output differs from the interpreter`);
    console.log(`PASS opt upstream checks ${name}`);
  }
}
