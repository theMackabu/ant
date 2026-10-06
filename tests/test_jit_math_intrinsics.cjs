// Compiled Math.<fn>(numbers) calls skip the native call when the callee is
// still the original builtin (see jit_emit_math_fastpath). Results must be
// the builtins' exactly: -0, NaN, round-half-up, ToInt32 in imul. Anything
// else, a replaced function or non-number arguments included, still calls.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

const out = new Array(8).fill(null);
const unary = {
  abs: (v, i) => (out[i & 7] = Math.abs(v)),
  ceil: (v, i) => (out[i & 7] = Math.ceil(v)),
  floor: (v, i) => (out[i & 7] = Math.floor(v)),
  round: (v, i) => (out[i & 7] = Math.round(v)),
  sign: (v, i) => (out[i & 7] = Math.sign(v)),
  sqrt: (v, i) => (out[i & 7] = Math.sqrt(v)),
  trunc: (v, i) => (out[i & 7] = Math.trunc(v)),
};
const binary = {
  imul: (a, b, i) => (out[i & 7] = Math.imul(a, b)),
  max: (a, b, i) => (out[i & 7] = Math.max(a, b)),
  min: (a, b, i) => (out[i & 7] = Math.min(a, b)),
};
function tailFloor(v) { return Math.floor(v); }

for (let i = 0; i < 5000; i++) {
  for (const f of Object.values(unary)) f(i * 0.25, i);
  for (const f of Object.values(binary)) f(i, 3, i);
  tailFloor(i * 0.5);
}

const cases = {
  abs: [[-5.5, 5.5], [-0, 0], [-Infinity, Infinity], [NaN, NaN], [3, 3]],
  ceil: [[-0.5, -0], [0.2, 1], [-0, -0], [-1.5, -1], [1e300, 1e300], [NaN, NaN], [-Infinity, -Infinity]],
  floor: [[-0.5, -1], [0.5, 0], [-0, -0], [1.5, 1], [-1e300, -1e300], [NaN, NaN], [Infinity, Infinity]],
  round: [
    [0.49999999999999994, 0], [0.5, 1], [-0.5, -0], [-0.4, -0], [2.5, 3], [-2.5, -2], [-2.6, -3],
    [1.5, 2], [-0, -0], [0, 0], [4503599627370495.5, 4503599627370496], [Infinity, Infinity], [NaN, NaN],
  ],
  sign: [[-0, -0], [0, 0], [-3, -1], [7.5, 1], [NaN, NaN], [-Infinity, -1]],
  sqrt: [[4, 2], [-1, NaN], [-0, -0], [Infinity, Infinity], [2, Math.SQRT2]],
  trunc: [[-0.5, -0], [2.7, 2], [-2.7, -2], [NaN, NaN], [1e21, 1e21]],
};
for (const [name, rows] of Object.entries(cases))
  for (const [i, [v, expected]] of rows.entries()) {
    same(unary[name](v, i), expected, `${name}(${v})`);
    same(out[i & 7], expected, `${name}(${v}) stored`);
  }

const pairs = {
  imul: [[0xffffffff, 5, -5], [2 ** 31, 2, 0], [3.7, 2, 6], [NaN, 1, 0], [Infinity, 1, 0], [-1, 8, -8], [-0, 3, 0]],
  max: [[0, -0, 0], [-0, 0, 0], [NaN, 1, NaN], [1, NaN, NaN], [-Infinity, -5, -5], [2, 1, 2]],
  min: [[0, -0, -0], [-0, 0, -0], [NaN, 1, NaN], [1, NaN, NaN], [Infinity, 5, 5], [2, 1, 1]],
};
for (const [name, rows] of Object.entries(pairs))
  for (const [i, [a, b, expected]] of rows.entries())
    same(binary[name](a, b, i), expected, `${name}(${a}, ${b})`);
same(tailFloor(-0.5), -1, 'tail floor');

// non-number arguments and other argument counts take the call
same(unary.abs('-3', 0), 3, 'abs string');
same(unary.floor({ valueOf: () => 2.5 }, 0), 2, 'floor object');
same(binary.max('5', 1, 0), 5, 'max string');
same(binary.imul(undefined, 3, 0), 0, 'imul undefined');
function maxOf3(a, b, c) { return Math.max(a, b, c); }
function absNone() { return Math.abs(); }
function minOne(a) { return Math.min(a); }
function imulOne(a) { return Math.imul(a); }
for (let i = 0; i < 5000; i++) { maxOf3(i, 1, 2); absNone(); minOne(i); imulOne(i); }
same(imulOne(3), 0, 'imul(a)');
same(maxOf3(1, 9, 3), 9, 'max of three');
same(absNone(), NaN, 'abs()');
same(minOne(4), 4, 'min(a)');

// inside helpers the JIT inlines into a compiled caller: in tail position,
// and stored to a local (an inlined call must be followed by such an op)
function hyp(x, y) { return Math.sqrt(x * x + y * y); }
function floorLocal(v) { const r = Math.floor(v); return r; }
function sumInlined(n) { let s = 0; for (let i = 0; i < n; i++) s += hyp(i, 1) + floorLocal(i * 0.5); return s; }
// .call always takes the generic call
let expectedSum = 0;
for (let i = 0; i < 200000; i++)
  expectedSum += Math.sqrt.call(null, i * i + 1) + Math.floor.call(null, i * 0.5);
same(sumInlined(200000), expectedSum, 'inlined Math calls');
const realSqrt = Math.sqrt;
Math.sqrt = () => 1;
try {
  same(sumInlined(4), 4 + 0 + 0 + 1 + 1, 'inlined replaced Math.sqrt');
} finally {
  Math.sqrt = realSqrt;
}

// replaced builtins are called
const realAbs = Math.abs;
Math.abs = () => 'replaced';
try {
  same(unary.abs(-1, 0), 'replaced', 'replaced Math.abs');
} finally {
  Math.abs = realAbs;
}
same(unary.abs(-1, 0), 1, 'restored Math.abs');

const realMath = globalThis.Math;
globalThis.Math = { floor: () => 'other Math', max: realMath.max };
try {
  same(unary.floor(1.5, 0), 'other Math', 'replaced Math');
} finally {
  globalThis.Math = realMath;
}
same(unary.floor(1.5, 0), 1, 'restored Math');

console.log('PASS compiled Math calls match the builtins');
