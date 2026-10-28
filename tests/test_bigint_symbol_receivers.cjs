function check(name, actual, expected) {
  const a = String(actual), e = String(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function throwsName(fn) {
  try { fn(); } catch (e) { return e.constructor.name; }
  return 'none';
}

function run() {
  const valueOf = BigInt.prototype.valueOf;
  const toString = BigInt.prototype.toString;
  const toLocaleString = BigInt.prototype.toLocaleString;

  check('valueOf number', throwsName(() => valueOf.call(1)), 'TypeError');
  check('valueOf object', throwsName(() => valueOf.call({})), 'TypeError');
  check('valueOf symbol', throwsName(() => valueOf.call(Symbol())), 'TypeError');
  check('valueOf undefined', throwsName(() => valueOf.call(undefined)), 'TypeError');
  check('valueOf fake proto', throwsName(() => valueOf.call(Object.create(BigInt.prototype))), 'TypeError');
  check('valueOf prototype', throwsName(() => valueOf.call(BigInt.prototype)), 'TypeError');
  check('valueOf proxy', throwsName(() => valueOf.call(new Proxy(Object(1n), {}))), 'TypeError');
  check('valueOf wrapper', valueOf.call(Object(5n)) === 5n, true);

  check('toString number', throwsName(() => toString.call(1)), 'TypeError');
  check('toString string', throwsName(() => toString.call('1')), 'TypeError');
  check('toString object', throwsName(() => toString.call({})), 'TypeError');
  check('toString bad receiver first', throwsName(() => toString.call(1, 99)), 'TypeError');
  check('toString wrapper radix', toString.call(Object(255n), 16), 'ff');
  check('toString radix 1', throwsName(() => (5n).toString(1)), 'RangeError');
  check('toString radix 37', throwsName(() => (5n).toString(37)), 'RangeError');
  check('toString radix 0', throwsName(() => (5n).toString(0)), 'RangeError');
  check('toString radix NaN', throwsName(() => (5n).toString(NaN)), 'RangeError');
  check('toString radix undefined', (255n).toString(undefined), '255');
  check('toString radix string', (255n).toString('16'), 'ff');
  check('toString radix fraction', (255n).toString(2.9), '11111111');
  check('toString radix object', (255n).toString({ valueOf() { return 8; } }), '377');
  check('toString radix bigint', throwsName(() => (5n).toString(10n)), 'TypeError');
  check('toString radix symbol', throwsName(() => (5n).toString(Symbol())), 'TypeError');
  check('toString negative', (-255n).toString(16), '-ff');
  check('toString radix 36', (35n).toString(36), 'z');

  check('toLocaleString own', Object.prototype.hasOwnProperty.call(BigInt.prototype, 'toLocaleString'), true);
  check('toLocaleString length', toLocaleString.length, 0);
  check('toLocaleString number', throwsName(() => toLocaleString.call(1)), 'TypeError');
  check('toLocaleString wrapper', toLocaleString.call(Object(7n)), '7');
  check('toLocaleString grouping', (1234567n).toLocaleString(), '1,234,567');
  check('toLocaleString negative', (-1234n).toLocaleString(), '-1,234');
  check('toLocaleString small', (123n).toLocaleString(), '123');
  check('toLocaleString zero', (0n).toLocaleString(), '0');
  check('toLocaleString enumerable', Object.keys(BigInt.prototype).length, 0);

  const symValueOf = Symbol.prototype.valueOf;
  const symToString = Symbol.prototype.toString;
  const description = Object.getOwnPropertyDescriptor(Symbol.prototype, 'description').get;
  const toPrimitive = Symbol.prototype[Symbol.toPrimitive];
  const sym = Symbol('q');

  check('symbol valueOf string', throwsName(() => symValueOf.call('x')), 'TypeError');
  check('symbol valueOf object', throwsName(() => symValueOf.call({})), 'TypeError');
  check('symbol valueOf undefined', throwsName(() => symValueOf.call(undefined)), 'TypeError');
  check('symbol valueOf fake proto', throwsName(() => symValueOf.call(Object.create(Symbol.prototype))), 'TypeError');
  check('symbol valueOf prototype', throwsName(() => symValueOf.call(Symbol.prototype)), 'TypeError');
  check('symbol valueOf wrapper', symValueOf.call(Object(sym)) === sym, true);
  check('symbol toString string', throwsName(() => symToString.call('x')), 'TypeError');
  check('symbol toString wrapper', symToString.call(Object(sym)), 'Symbol(q)');
  check('symbol description string', throwsName(() => description.call('x')), 'TypeError');
  check('symbol description wrapper', description.call(Object(sym)), 'q');
  check('symbol toPrimitive string', throwsName(() => toPrimitive.call('x')), 'TypeError');
  check('symbol toPrimitive wrapper', toPrimitive.call(Object(sym)) === sym, true);
}

for (let i = 0; i < 40; i++) run();
console.log('ok');
