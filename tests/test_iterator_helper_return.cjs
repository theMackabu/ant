const assert = require('node:assert');

function source(log, options = {}) {
  let index = 0;
  const iterator = {
    next() { log.push('next'); return { value: index++, done: index > (options.length ?? 5) }; },
    [Symbol.iterator]() { return this; },
  };
  if (!options.noReturn) iterator.return = function () {
    log.push('return');
    if (options.throwOnReturn) throw new RangeError('return');
    return options.badReturn ? 1 : {};
  };
  return iterator;
}

const helpers = {
  map: (s) => Iterator.from(s).map((x) => x),
  filter: (s) => Iterator.from(s).filter(() => true),
  take: (s) => Iterator.from(s).take(3),
  drop: (s) => Iterator.from(s).drop(1),
  flatMap: (s) => Iterator.from(s).flatMap((x) => [x, x]),
};

for (const [name, make] of Object.entries(helpers)) {
  {
    const helper = make(source([]));
    const proto = Object.getPrototypeOf(helper);
    assert.deepStrictEqual(Reflect.ownKeys(proto), ['next', 'return', Symbol.toStringTag], name);
    assert.strictEqual(Object.prototype.toString.call(helper), '[object Iterator Helper]', name);
    assert.strictEqual(Object.getPrototypeOf(proto), Iterator.prototype, name);
  }
  {
    const log = [];
    const helper = make(source(log));
    assert.deepStrictEqual(helper.return(), { value: undefined, done: true }, name);
    assert.deepStrictEqual(log, ['return'], name);
    assert.deepStrictEqual(helper.next(), { value: undefined, done: true }, name);
    assert.deepStrictEqual(log, ['return'], name);
  }
  {
    const log = [];
    const helper = make(source(log));
    helper.next();
    helper.return();
    helper.return();
    assert.strictEqual(log.filter((entry) => entry === 'return').length, 1, name);
    assert.strictEqual(helper.next().done, true, name);
  }
  {
    const helper = make(source([], { noReturn: true }));
    helper.next();
    assert.deepStrictEqual(helper.return(), { value: undefined, done: true }, name);
  }
  {
    const helper = make(source([], { throwOnReturn: true }));
    helper.next();
    assert.throws(() => helper.return(), RangeError, name);
    assert.strictEqual(helper.next().done, true, name);
  }
  {
    const helper = make(source([], { badReturn: true }));
    helper.next();
    assert.throws(() => helper.return(), TypeError, name);
  }
  {
    const log = [];
    for (const value of make(source(log))) break;
    assert.strictEqual(log.filter((entry) => entry === 'return').length, 1, name);
  }
  {
    const log = [];
    const helper = make(source(log, { length: 2 }));
    while (!helper.next().done);
    const before = log.length;
    helper.return();
    assert.strictEqual(log.length, before, name);
  }
}

{
  const log = [];
  const helper = Iterator.from(source(log)).take(2);
  assert.deepStrictEqual([helper.next().value, helper.next().value, helper.next().done], [0, 1, true]);
  assert.deepStrictEqual(log, ['next', 'next', 'return']);
}

{
  const log = [];
  const inner = { next() { return { value: 1, done: false }; }, return() { log.push('inner'); return {}; }, [Symbol.iterator]() { return this; } };
  const outer = { next() { return { value: 0, done: false }; }, return() { log.push('outer'); return {}; }, [Symbol.iterator]() { return this; } };
  const helper = Iterator.from(outer).flatMap(() => inner);
  helper.next();
  helper.return();
  assert.deepStrictEqual(log, ['inner', 'outer']);
}

{
  const helperReturn = Object.getPrototypeOf([1].values().map((x) => x)).return;
  assert.throws(() => helperReturn.call({}), TypeError);
  assert.throws(() => helperReturn.call([1].values()), TypeError);
}

{
  const wrapped = Iterator.from({ next() { return { done: true }; } });
  assert.strictEqual(Object.prototype.toString.call(wrapped), '[object Iterator]');
  assert.strictEqual(typeof wrapped.return, 'function');
  assert.strictEqual(Object.prototype.toString.call([1].values()), '[object Array Iterator]');
}

console.log('ok');
