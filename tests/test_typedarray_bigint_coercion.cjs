let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

function outcome(fn) {
  try { return fn(); } catch (e) { return e; }
}

function expectThrow(fn, type, msg) {
  const r = outcome(fn);
  check(r instanceof type, `${msg}: expected ${type.name}, got ${r}`);
}

function expectArray(fn, want, msg) {
  const r = outcome(fn);
  const got = r && typeof r.length === 'number' ? Array.from(r) : r;
  const ok = Array.isArray(got) && got.length === want.length && got.every((v, i) => Object.is(v, want[i]));
  check(ok, `${msg}: expected [${want.map(String)}], got ${Array.isArray(got) ? '[' + got.map(String) + ']' : got}`);
}

const numberTypes = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array];
const bigTypes = [BigInt64Array, BigUint64Array];
const big7 = { valueOf() { return 7n; } };
const num7 = { valueOf() { return 7; } };

for (const C of numberTypes) {
  const n = C.name;
  expectThrow(() => { const a = new C(2); a[0] = 1n; }, TypeError, `${n} element store of a BigInt`);
  expectThrow(() => new C(2).fill(1n), TypeError, `${n}.fill(BigInt)`);
  expectThrow(() => new C(2).fill(big7), TypeError, `${n}.fill(object returning BigInt)`);
  expectArray(() => new C(2).fill('5'), [5, 5], `${n}.fill(string)`);
  expectArray(() => new C(2).fill(num7), [7, 7], `${n}.fill(object returning number)`);
  expectThrow(() => new C(2).set([1n, 2n]), TypeError, `${n}.set(array of BigInt)`);
  expectThrow(() => new C(2).set({ length: 2, 0: 1, 1: 2n }), TypeError, `${n}.set(array-like with BigInt)`);
  expectThrow(() => new C(2).set(new BigInt64Array([1n, 2n])), TypeError, `${n}.set(BigInt64Array)`);
  expectThrow(() => C.from([1n, 2n]), TypeError, `${n}.from(BigInt array)`);
  expectThrow(() => C.from([1, 2], x => BigInt(x)), TypeError, `${n}.from with BigInt mapper`);
  expectThrow(() => C.from(new BigInt64Array([3n])), TypeError, `${n}.from(BigInt64Array)`);
  expectThrow(() => C.of(1n, 2n), TypeError, `${n}.of(BigInt)`);
  expectThrow(() => new C([1n, 2n]), TypeError, `new ${n}(BigInt array)`);
  expectThrow(() => new C(new Set([1n])), TypeError, `new ${n}(BigInt iterable)`);
  expectThrow(() => new C({ length: 1, 0: 1n }), TypeError, `new ${n}(BigInt array-like)`);
  expectThrow(() => new C(new BigInt64Array([1n])), TypeError, `new ${n}(BigInt64Array)`);
  expectThrow(() => new C(1).map(() => 1n), TypeError, `${n}.map returning BigInt`);
  expectThrow(() => new C(1).with(0, 1n), TypeError, `${n}.with(BigInt)`);
  expectArray(() => C.from([1, 2]), [1, 2], `${n}.from(numbers)`);
  expectArray(() => C.of(3), [3], `${n}.of(number)`);
  expectArray(() => new C(['4']), [4], `new ${n}(string array)`);
}

expectArray(() => new Float64Array(2).fill(), [NaN, NaN], 'Float64Array.fill() writes undefined as NaN');
expectArray(() => new Int8Array(2).fill(), [0, 0], 'Int8Array.fill() writes undefined as 0');

for (const C of bigTypes) {
  const n = C.name;
  expectThrow(() => { const a = new C(2); a[0] = 1; }, TypeError, `${n} element store of a Number`);
  expectArray(() => { const a = new C(1); a[0] = '3'; return a; }, [3n], `${n} element store of a string`);
  expectArray(() => { const a = new C(1); a[0] = true; return a; }, [1n], `${n} element store of a boolean`);
  expectArray(() => { const a = new C(1); a[0] = big7; return a; }, [7n], `${n} element store of an object returning BigInt`);
  expectThrow(() => { const a = new C(1); a[0] = 'x'; }, SyntaxError, `${n} element store of an unparsable string`);
  expectThrow(() => { const a = new C(1); a[0] = undefined; }, TypeError, `${n} element store of undefined`);
  expectThrow(() => new C(2).fill(1), TypeError, `${n}.fill(Number)`);
  expectThrow(() => new C(2).fill(), TypeError, `${n}.fill() with undefined`);
  expectArray(() => new C(2).fill('5'), [5n, 5n], `${n}.fill(string)`);
  expectArray(() => new C(2).fill(true), [1n, 1n], `${n}.fill(boolean)`);
  expectArray(() => new C(2).fill(big7), [7n, 7n], `${n}.fill(object returning BigInt)`);
  expectThrow(() => new C(2).set([1, 2]), TypeError, `${n}.set(array of Number)`);
  expectArray(() => { const a = new C(2); a.set(['1', '2']); return a; }, [1n, 2n], `${n}.set(array of strings)`);
  expectThrow(() => new C(2).set(new Int8Array([1, 2])), TypeError, `${n}.set(Int8Array)`);
  expectThrow(() => C.from([1, 2]), TypeError, `${n}.from(numbers)`);
  expectThrow(() => C.of(1, 2), TypeError, `${n}.of(numbers)`);
  expectThrow(() => new C([1, 2]), TypeError, `new ${n}(number array)`);
  expectThrow(() => new C(new Set([1])), TypeError, `new ${n}(number iterable)`);
  expectThrow(() => new C({ length: 1, 0: 1 }), TypeError, `new ${n}(number array-like)`);
  expectThrow(() => new C(new Int8Array([1])), TypeError, `new ${n}(Int8Array)`);
  expectThrow(() => new C(1).map(() => 1), TypeError, `${n}.map returning Number`);
  expectThrow(() => new C(1).with(0, 1), TypeError, `${n}.with(Number)`);
  expectArray(() => C.from([1n, 2n]), [1n, 2n], `${n}.from(BigInts)`);
  expectArray(() => C.of(1n), [1n], `${n}.of(BigInt)`);
  expectArray(() => new C(['4']), [4n], `new ${n}(string array)`);
  expectArray(() => new C(new BigInt64Array([5n])), [5n], `new ${n}(BigInt64Array)`);
}

expectThrow(() => new DataView(new ArrayBuffer(8)).setInt8(0, 1n), TypeError, 'DataView.setInt8(BigInt)');
expectThrow(() => new DataView(new ArrayBuffer(8)).setFloat64(0, 1n), TypeError, 'DataView.setFloat64(BigInt)');
expectThrow(() => new DataView(new ArrayBuffer(8)).setBigInt64(0, 1), TypeError, 'DataView.setBigInt64(Number)');
check(outcome(() => { const d = new DataView(new ArrayBuffer(8)); d.setBigInt64(0, '9'); return d.getBigInt64(0); }) === 9n, 'DataView.setBigInt64(string)');

const atomicNumberTypes = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];
for (const C of atomicNumberTypes) {
  const n = C.name;
  const make = () => new C(new SharedArrayBuffer(C.BYTES_PER_ELEMENT * 2));
  for (const op of ['store', 'add', 'sub', 'and', 'or', 'xor', 'exchange']) {
    expectThrow(() => Atomics[op](make(), 0, 1n), TypeError, `Atomics.${op}(${n}, BigInt)`);
  }
  expectThrow(() => Atomics.compareExchange(make(), 0, 0, 5n), TypeError, `Atomics.compareExchange(${n}, Number, BigInt)`);
  expectThrow(() => Atomics.compareExchange(make(), 0, 0n, 5), TypeError, `Atomics.compareExchange(${n}, BigInt, Number)`);
  check(outcome(() => Atomics.store(make(), 0, '2')) === 2, `Atomics.store(${n}, string) returns the integer`);
  check(outcome(() => Atomics.store(make(), 0, 3.7)) === 3, `Atomics.store(${n}, 3.7) returns 3`);
  expectArray(() => { const a = make(); Atomics.add(a, 0, 5); Atomics.sub(a, 0, 2); return a; }, [3, 0], `Atomics.add/sub on ${n}`);
  check(outcome(() => { const a = make(); a[0] = 6; return Atomics.compareExchange(a, 0, 6, 9) === 6 && a[0] === 9; }) === true, `Atomics.compareExchange on ${n}`);
}
check(outcome(() => { const a = new Uint32Array(1); return Atomics.exchange(a, 0, 0xffffffff) === 0 && Atomics.load(a, 0) === 0xffffffff; }) === true, 'Uint32 atomics keep unsigned values');
check(outcome(() => { const a = new Int8Array(1); Atomics.store(a, 0, 200); return Atomics.load(a, 0); }) === -56, 'Int8 atomics wrap stores');

for (const C of bigTypes) {
  const n = C.name;
  const make = () => new C(new SharedArrayBuffer(16));
  for (const op of ['store', 'add', 'sub', 'and', 'or', 'xor', 'exchange']) {
    expectThrow(() => Atomics[op](make(), 0, 1), TypeError, `Atomics.${op}(${n}, Number)`);
  }
  expectThrow(() => Atomics.compareExchange(make(), 0, 0n, 5), TypeError, `Atomics.compareExchange(${n}, BigInt, Number)`);
  check(outcome(() => Atomics.store(make(), 0, 1n)) === 1n, `Atomics.store(${n}, BigInt) returns the BigInt`);
  check(outcome(() => Atomics.store(make(), 0, '2')) === 2n, `Atomics.store(${n}, string) converts with ToBigInt`);
  expectArray(() => { const a = make(); Atomics.add(a, 0, 5n); Atomics.sub(a, 0, 2n); Atomics.or(a, 1, 12n); Atomics.and(a, 1, 10n); Atomics.xor(a, 1, 1n); return a; }, [3n, 9n], `Atomics RMW on ${n}`);
  check(outcome(() => { const a = make(); a[0] = 6n; return Atomics.compareExchange(a, 0, 6n, 9n) === 6n && Atomics.load(a, 0) === 9n; }) === true, `Atomics.compareExchange on ${n}`);
  check(outcome(() => { const a = make(); return Atomics.exchange(a, 0, 4n) === 0n && Atomics.load(a, 0) === 4n; }) === true, `Atomics.exchange on ${n}`);
}
check(outcome(() => { const a = new BigUint64Array(1); Atomics.sub(a, 0, 1n); return a[0]; }) === 18446744073709551615n, 'BigUint64 atomics wrap');
check(outcome(() => { const a = new BigInt64Array(1); Atomics.sub(a, 0, 1n); return a[0]; }) === -1n, 'BigInt64 atomics are signed');
expectThrow(() => Atomics.store(new Float64Array(1), 0, 1), TypeError, 'Atomics.store on Float64Array');
expectThrow(() => Atomics.store(new Uint8ClampedArray(1), 0, 1), TypeError, 'Atomics.store on Uint8ClampedArray');
expectThrow(() => Atomics.load(new Int32Array(1), 1), RangeError, 'Atomics.load out of range');

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
