const assert = require('node:assert');

function source(log, options = {}) {
  let index = 0;
  return {
    next() {
      log.push('next');
      if (options.throwOnNext) throw new RangeError('next');
      return { value: index++, done: false };
    },
    return() {
      log.push('return');
      if (options.throwOnReturn) throw new SyntaxError('return');
      return {};
    },
  };
}

for (const name of ['map', 'filter', 'flatMap']) {
  {
    const log = [];
    const helper = Iterator.from(source(log))[name](() => { throw new RangeError('callback'); });
    assert.throws(() => helper.next(), RangeError, name);
    assert.deepStrictEqual(log, ['next', 'return'], name);
    assert.deepStrictEqual(helper.next(), { value: undefined, done: true }, name);
    assert.deepStrictEqual(helper.return(), { value: undefined, done: true }, name);
    assert.deepStrictEqual(log, ['next', 'return'], name);
  }
  {
    const log = [];
    const helper = Iterator.from(source(log, { throwOnReturn: true }))[name](() => { throw new RangeError('callback'); });
    assert.throws(() => helper.next(), RangeError, name);
    assert.deepStrictEqual(log, ['next', 'return'], name);
  }
  {
    const log = [];
    const helper = Iterator.from(source(log, { throwOnNext: true }))[name]((value) => value);
    assert.throws(() => helper.next(), RangeError, name);
    assert.deepStrictEqual(log, ['next'], name);
    assert.deepStrictEqual(helper.next(), { value: undefined, done: true }, name);
    assert.deepStrictEqual(log, ['next'], name);
  }
  {
    const log = [];
    let calls = 0;
    const helper = Iterator.from(source(log))[name]((value) => {
      if (calls++ === 1) throw new RangeError('second');
      return name === 'flatMap' ? [value] : true;
    });
    assert.strictEqual(helper.next().done, false, name);
    assert.throws(() => helper.next(), RangeError, name);
    assert.deepStrictEqual(log, ['next', 'next', 'return'], name);
  }
}

for (const inner of [
  { next() { throw new RangeError('inner next'); }, [Symbol.iterator]() { return this; } },
  { next() { return { get done() { throw new RangeError('inner done'); } }; }, [Symbol.iterator]() { return this; } },
  { next() { return { done: false, get value() { throw new RangeError('inner value'); } }; }, [Symbol.iterator]() { return this; } },
  { [Symbol.iterator]() { throw new RangeError('inner iterator'); } },
]) {
  const log = [];
  const helper = Iterator.from(source(log)).flatMap(() => inner);
  assert.throws(() => helper.next(), RangeError);
  assert.deepStrictEqual(log, ['next', 'return']);
  assert.strictEqual(helper.next().done, true);
}

{
  const log = [];
  let calls = 0;
  const inner = { next() { if (calls++ === 1) throw new RangeError('inner'); return { value: 1, done: false }; }, return() { log.push('inner return'); return {}; }, [Symbol.iterator]() { return this; } };
  const helper = Iterator.from(source(log)).flatMap(() => inner);
  assert.strictEqual(helper.next().value, 1);
  assert.throws(() => helper.next(), RangeError);
  assert.deepStrictEqual(log, ['next', 'return']);
}

{
  const generator = (function* () { yield 1; })();
  assert.strictEqual(Iterator.from(generator), generator);

  const helper = [1].values().map((value) => value);
  assert.strictEqual(Iterator.from(helper), helper);

  class Custom extends Iterator { next() { return { value: 1, done: false }; } }
  const custom = new Custom();
  assert.strictEqual(Iterator.from(custom), custom);

  const array = [1, 2];
  const fromArray = Iterator.from(array);
  assert.strictEqual(Object.prototype.toString.call(fromArray), '[object Array Iterator]');
  assert.deepStrictEqual([...Iterator.from('ab')], ['a', 'b']);

  const both = { next() { return { value: 9, done: false }; }, [Symbol.iterator]() { return [5].values(); } };
  assert.strictEqual(Iterator.from(both).next().value, 5);

  const plain = { [Symbol.iterator]() { return { next() { return { value: 7, done: false }; } }; } };
  const wrapped = Iterator.from(plain);
  assert.strictEqual(wrapped.next().value, 7);
  assert.strictEqual(Object.getPrototypeOf(Object.getPrototypeOf(wrapped)), Iterator.prototype);
  assert.strictEqual(typeof wrapped.return, 'function');

  const wrappedAgain = Iterator.from(wrapped);
  assert.strictEqual(wrappedAgain, wrapped);

  for (const value of [1, null, undefined, true, Symbol('s')]) assert.throws(() => Iterator.from(value), TypeError);
  assert.throws(() => Iterator.from({ [Symbol.iterator]() { return 1; } }), TypeError);
  assert.throws(() => Iterator.from({ [Symbol.iterator]: 1 }), TypeError);

  const empty = Iterator.from({});
  assert.throws(() => empty.next(), TypeError);
}

console.log('ok');
