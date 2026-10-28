let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

function throwsType(fn, msg) {
  try { fn(); } catch (e) {
    check(e instanceof TypeError, msg + ': expected TypeError, got ' + e);
    return;
  }
  check(false, msg + ': expected TypeError');
}

const valueOf = Object.prototype.valueOf;
for (const [v, Wrapper] of [[1, Number], ['ab', String], [true, Boolean], [Symbol.iterator, Symbol], [2n, BigInt]]) {
  const boxed = valueOf.call(v);
  check(typeof boxed === 'object', `valueOf.call(${String(v)}) returns an object, got ${typeof boxed}`);
  check(boxed instanceof Wrapper, `valueOf.call(${String(v)}) is a ${Wrapper.name} wrapper`);
  check(Object.getPrototypeOf(boxed) === Wrapper.prototype, `valueOf.call(${String(v)}) has the wrapper prototype`);
  check(boxed.valueOf() === v, `valueOf.call(${String(v)}) wraps the primitive`);
}
const o = {};
check(valueOf.call(o) === o, 'valueOf returns objects unchanged');
const f = function () {};
check(valueOf.call(f) === f, 'valueOf returns functions unchanged');
throwsType(() => valueOf.call(null), 'valueOf.call(null)');
throwsType(() => valueOf.call(undefined), 'valueOf.call(undefined)');

const pie = Object.prototype.propertyIsEnumerable;
check(pie.call('abc', 0) === true, 'string index is enumerable');
check(pie.call('abc', '2') === true, 'last string index is enumerable');
check(pie.call('abc', 3) === false, 'index past the end is not enumerable');
check(pie.call('abc', 'length') === false, 'string length is not enumerable');
check(pie.call('\u{1F600}', 1) === true, 'string indexes count UTF-16 units');
check(pie.call(1, 'x') === false, 'number has no own enumerable properties');
throwsType(() => pie.call(null, 'x'), 'propertyIsEnumerable.call(null)');
throwsType(() => pie.call(undefined, 'x'), 'propertyIsEnumerable.call(undefined)');
check(pie.call({ a: 1 }, 'a') === true, 'own enumerable property');

const ipo = Object.prototype.isPrototypeOf;
throwsType(() => ipo.call(null, {}), 'isPrototypeOf.call(null, object)');
throwsType(() => ipo.call(undefined, {}), 'isPrototypeOf.call(undefined, object)');
check(ipo.call(null, 1) === false, 'isPrototypeOf with a primitive argument is false before ToObject');
check(ipo.call(undefined) === false, 'isPrototypeOf with no argument is false');
check(ipo.call(Object.prototype, {}) === true, 'Object.prototype is a prototype of {}');
check(ipo.call(Number.prototype, 1) === false, 'primitives have no prototype chain for isPrototypeOf');

const hop = Object.prototype.hasOwnProperty;
check(hop.call('abc', 'length') === true, 'string has own length');
check(hop.call('abc', 1) === true, 'string has own index');
throwsType(() => hop.call(null, 'x'), 'hasOwnProperty.call(null)');

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
