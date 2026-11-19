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
  assert.deepStrictEqual(wrapped.return(), { value: undefined, done: true });
  assert.strictEqual(Object.prototype.toString.call([1].values()), '[object Array Iterator]');
}

{
  const log = [];
  const closed = { closed: true };
  const steps = [{ value: 1, done: false }, { value: 2, done: true }, { value: 3, done: false }];
  let index = 0;
  const wrapped = Iterator.from({
    next() { log.push('next'); return steps[index++]; },
    return() { log.push('return'); return closed; },
  });
  assert.strictEqual(wrapped.next(), steps[0]);
  assert.strictEqual(wrapped.return(), closed);
  assert.strictEqual(wrapped.return(), closed);
  assert.strictEqual(wrapped.next(), steps[1]);
  assert.strictEqual(wrapped.next(), steps[2]);
  assert.deepStrictEqual(log, ['next', 'return', 'return', 'next', 'next']);

  const proto = Object.getPrototypeOf(wrapped);
  assert.throws(() => proto.next.call({}), TypeError);
  assert.throws(() => proto.return.call([1].values().map((x) => x)), TypeError);
  assert.throws(() => Object.getPrototypeOf([1].values().map((x) => x)).next.call(wrapped), TypeError);
}

{
  let reads = 0;
  const base = { get next() { reads++; return () => ({ value: 1, done: false }); } };
  const wrapped = Iterator.from(base);
  wrapped.next();
  wrapped.next();
  assert.strictEqual(reads, 1);

  reads = 0;
  const helper = Iterator.prototype.map.call(base, (x) => x);
  helper.next();
  helper.next();
  assert.strictEqual(reads, 1);
}

for (const name of ['map', 'filter', 'flatMap']) {
  const log = [];
  const errors = [];
  const helper = Iterator.from(source(log))[name]((value) => {
    try { helper.return(); } catch (error) { errors.push(error); }
    try { helper.next(); } catch (error) { errors.push(error); }
    return name === 'flatMap' ? [value] : true;
  });
  assert.strictEqual(helper.next().done, false, name);
  assert.strictEqual(helper.next().done, false, name);
  assert.deepStrictEqual(log, ['next', 'next'], name);
  assert.strictEqual(errors.length, 4, name);
  for (const error of errors) assert.ok(error instanceof TypeError, name);
  helper.return();
  assert.deepStrictEqual(log, ['next', 'next', 'return'], name);
}

{
  const log = [];
  const helper = Iterator.from({
    next() {
      log.push('next');
      try { helper.return(); } catch (error) { log.push(error.constructor.name); }
      return { value: 1, done: false };
    },
    return() { log.push('return'); return {}; },
  }).take(2);
  assert.deepStrictEqual(helper.next(), { value: 1, done: false });
  assert.deepStrictEqual(log, ['next', 'TypeError']);
}

{
  const log = [];
  const inner = { next() { return { value: 1, done: false }; }, return() { log.push('inner'); throw new RangeError('inner'); }, [Symbol.iterator]() { return this; } };
  const outer = { next() { return { value: 0, done: false }; }, return() { log.push('outer'); throw new SyntaxError('outer'); } };
  const helper = Iterator.from(outer).flatMap(() => inner);
  helper.next();
  assert.throws(() => helper.return(), RangeError);
  assert.deepStrictEqual(log, ['inner', 'outer']);
  assert.strictEqual(helper.next().done, true);

  let caught = null;
  try { helper.return(); throw new EvalError('later'); } catch (error) { caught = error; }
  assert.ok(caught instanceof EvalError);
}

{
  const log = [];
  const inner = { next() { return { value: 1, done: false }; }, return() { return 1; }, [Symbol.iterator]() { return this; } };
  const outer = { next() { return { value: 0, done: false }; }, return() { log.push('outer'); return {}; } };
  const helper = Iterator.from(outer).flatMap(() => inner);
  helper.next();
  assert.throws(() => helper.return(), TypeError);
  assert.deepStrictEqual(log, ['outer']);
}

console.log('ok');
