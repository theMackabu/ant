import { test, summary } from './helpers.js';

console.log('Optional Chaining Tests\n');

const u = undefined;
test('member: u?.b', u?.b, undefined);
test('member: u?.b.c', u?.b.c, undefined);
test('member: u?.b.c.d', u?.b.c.d, undefined);
test('member: u?.[0].x', u?.[0].x, undefined);

test('call: u?.b()', u?.b(), undefined);
test('call: u?.b.c()', u?.b.c(), undefined);
test('call: u?.b.c.d()', u?.b.c.d(), undefined);
test('call: u?.b[0]()', u?.b[0](), undefined);
test('call: u?.b.c?.()', u?.b.c?.(), undefined);
test("call: u?.['b'].c()", u?.['b'].c(), undefined);
test('call: null?.b.c()', null?.b.c(), undefined);

test('intrinsic: u?.b.includes(1)', u?.b.includes(1), undefined);
test('intrinsic: u?.b.isPrototypeOf({})', u?.b.isPrototypeOf({}), undefined);
test('intrinsic: truthy u?.b.exec()', u?.b.exec('x') ? 1 : 2, 2);

const o = { b: { c: () => 42, arr: [1, 2, 3] } };
test('present: o?.b.c()', o?.b.c(), 42);
test('present: o?.b.c?.()', o?.b.c?.(), 42);
test("present: o?.b['c']()", o?.b['c'](), 42);
test('present: o?.b.arr.includes(2)', o?.b.arr.includes(2), true);
test('present: o?.b.arr.includes(9)', o?.b.arr.includes(9), false);
test("present: 'abc'?.includes('b')", 'abc'?.includes('b'), true);

test('tail: (() => u?.b.c())()', (() => u?.b.c())(), undefined);
test('tail: (() => o?.b.c())()', (() => o?.b.c())(), 42);

let called = false;
const mark = () => {
  called = true;
  return 1;
};
undefined?.b.c(mark());
test('short-circuit skips arguments', called, false);
called = false;
o?.b.arr.includes(mark());
test('live call evaluates arguments', called, true);

const obj = {
  n: 7,
  get() {
    return this.n;
  }
};
test('this binding: obj?.get()', obj?.get(), 7);
test('this binding: nested chain', { inner: obj }?.inner.get(), 7);

summary();
