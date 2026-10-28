function check(name, actual, expected) {
  const a = String(actual), e = String(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function throwsName(fn) {
  try { fn(); } catch (e) { return e.constructor.name; }
  return 'none';
}

function run() {
  check('int8 includes bigint', new Int8Array(1).includes(0n), false);
  check('int8 indexOf bigint', new Int8Array(1).indexOf(0n), -1);
  check('int8 lastIndexOf bigint', new Int8Array(1).lastIndexOf(0n), -1);
  check('uint8clamped includes bigint', new Uint8ClampedArray([1]).includes(1n), false);
  check('float64 includes bigint', new Float64Array([1]).includes(1n), false);

  check('bigint64 includes number', new BigInt64Array(1).includes(0), false);
  check('bigint64 indexOf number', new BigInt64Array(1).indexOf(0), -1);
  check('bigint64 lastIndexOf number', new BigInt64Array(1).lastIndexOf(0), -1);
  check('bigint64 includes bigint', new BigInt64Array(1).includes(0n), true);
  check('bigint64 indexOf bigint', new BigInt64Array([1n, 0n]).indexOf(0n), 1);
  check('bigint64 lastIndexOf bigint', new BigInt64Array([0n, 0n]).lastIndexOf(0n), 1);
  check('bigint64 negative', new BigInt64Array([-1n]).includes(-1n), true);
  check('bigint64 no wrap', new BigInt64Array([-1n]).includes(2n ** 64n - 1n), false);
  check('bigint64 out of range', new BigInt64Array([0n]).includes(2n ** 64n), false);
  check('biguint64 max', new BigUint64Array([2n ** 64n - 1n]).includes(2n ** 64n - 1n), true);
  check('biguint64 no wrap', new BigUint64Array([2n ** 64n - 1n]).includes(-1n), false);
  check('biguint64 indexOf', new BigUint64Array([5n, 2n ** 63n]).indexOf(2n ** 63n), 1);

  check('float64 includes NaN', new Float64Array([NaN]).includes(NaN), true);
  check('float64 indexOf NaN', new Float64Array([NaN]).indexOf(NaN), -1);
  check('float64 lastIndexOf NaN', new Float64Array([NaN]).lastIndexOf(NaN), -1);
  check('float32 includes NaN', new Float32Array([1, NaN]).includes(NaN), true);
  check('int8 includes NaN', new Int8Array([0]).includes(NaN), false);
  check('float64 includes -0', new Float64Array([-0]).includes(0), true);
  check('float64 indexOf -0', new Float64Array([0]).indexOf(-0), 0);
  check('float64 lastIndexOf -0', new Float64Array([0]).lastIndexOf(-0), 0);
  check('int8 indexOf -0', new Int8Array([0]).indexOf(-0), 0);

  check('int8 fraction', new Int8Array([1]).includes(1.5), false);
  check('int8 out of range', new Int8Array([44]).includes(300), false);
  check('int8 indexOf out of range', new Int8Array([44]).indexOf(300), -1);
  check('uint8 negative', new Uint8Array([255]).indexOf(-1), -1);
  check('uint32 max', new Uint32Array([4294967295]).indexOf(4294967295), 0);
  check('int32 min', new Int32Array([-2147483648]).lastIndexOf(-2147483648), 0);
  check('float32 precision', new Float32Array([0.1]).includes(0.1), false);
  check('float32 exact', new Float32Array([0.5]).indexOf(0.5), 0);
  check('float32 infinity', new Float32Array([Infinity]).indexOf(Infinity), 0);

  check('uint8 string', new Uint8Array([1]).includes('1'), false);
  check('uint8 object', new Uint8Array([1]).indexOf({ valueOf() { return 1; } }), -1);
  check('int8 includes no args', new Int8Array(1).includes(), false);
  check('float64 includes no args', new Float64Array([NaN]).includes(), false);
  check('bigint64 includes 1.5', new BigInt64Array([1n]).includes(1.5), false);
  check('bigint64 includes 1', new BigInt64Array([1n]).includes(1), false);
  check('empty includes', new Int8Array(0).includes(0n), false);

  const a = new Int16Array([1, 2, 1, 2]);
  check('indexOf from', a.indexOf(1, 1), 2);
  check('indexOf negative from', a.indexOf(2, -1), 3);
  check('indexOf infinity', a.indexOf(1, Infinity), -1);
  check('indexOf -infinity', a.indexOf(1, -Infinity), 0);
  check('indexOf NaN from', a.indexOf(1, NaN), 0);
  check('lastIndexOf from', a.lastIndexOf(2, 2), 1);
  check('lastIndexOf undefined from', a.lastIndexOf(1, undefined), 0);
  check('lastIndexOf negative from', a.lastIndexOf(1, -2), 2);
  check('lastIndexOf -infinity', a.lastIndexOf(1, -Infinity), -1);
  check('lastIndexOf infinity', a.lastIndexOf(2, Infinity), 3);
  check('includes from', a.includes(1, 3), false);
  check('includes string from', a.includes(2, '3'), true);
  check('from valueOf throws', throwsName(() => a.indexOf(1, { valueOf() { throw new SyntaxError('x'); } })), 'SyntaxError');
  check('from symbol', throwsName(() => a.includes(1, Symbol())), 'TypeError');
  check('from bigint', throwsName(() => a.lastIndexOf(1, 1n)), 'TypeError');
  check('bad receiver', throwsName(() => Int8Array.prototype.includes.call([1], 1)), 'TypeError');
}

for (let i = 0; i < 40; i++) run();
console.log('ok');
