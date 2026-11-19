const assert = require('node:assert');

function tracked(log, id, length = 2, options = {}) {
  return {
    [Symbol.iterator]() {
      log.push(id + ':open');
      let index = 0;
      const iterator = {
        next() {
          log.push(id + ':next');
          if (options.throwAt === index) throw new RangeError('next');
          return options.badStep ? 5 : { value: id + index, done: index++ >= length };
        },
      };
      if (!options.noReturn) iterator.return = () => { log.push(id + ':return'); return {}; };
      return iterator;
    },
  };
}

assert.deepStrictEqual([...Iterator.concat([1, 2], new Set([3]), (function* () { yield 4; })())], [1, 2, 3, 4]);
assert.deepStrictEqual(Iterator.concat().next(), { value: undefined, done: true });
assert.deepStrictEqual([...Iterator.concat([], [])], []);
assert.deepStrictEqual([Iterator.concat.name, Iterator.concat.length], ['concat', 0]);

{
  const joined = Iterator.concat([1]);
  assert.strictEqual(Object.prototype.toString.call(joined), '[object Iterator Helper]');
  assert.strictEqual(Object.getPrototypeOf(joined), Object.getPrototypeOf([].values().map((x) => x)));
  assert.strictEqual(joined[Symbol.iterator](), joined);
}

for (const bad of ['abc', 5, null, undefined, {}, { [Symbol.iterator]: 5 }, { [Symbol.iterator]: null }])
  assert.throws(() => Iterator.concat([1], bad), TypeError);

{
  const log = [];
  assert.throws(() => Iterator.concat(
    { get [Symbol.iterator]() { log.push('first'); return function () {}; } }, 5,
    { get [Symbol.iterator]() { log.push('third'); return function () {}; } },
  ), TypeError);
  assert.deepStrictEqual(log, ['first']);
}

{
  const log = [];
  const joined = Iterator.concat(tracked(log, 'a', 1), tracked(log, 'b', 1));
  assert.deepStrictEqual(log, []);
  assert.deepStrictEqual([...joined], ['a0', 'b0']);
  assert.deepStrictEqual(log, ['a:open', 'a:next', 'a:next', 'b:open', 'b:next', 'b:next']);
}

{
  let methodReads = 0, nextReads = 0;
  const iterable = {
    get [Symbol.iterator]() {
      methodReads++;
      return () => ({ get next() { nextReads++; let i = 0; return () => ({ value: i, done: i++ > 2 }); } });
    },
  };
  assert.deepStrictEqual([...Iterator.concat(iterable)], [0, 1, 2]);
  assert.deepStrictEqual([methodReads, nextReads], [1, 1]);
}

{
  const log = [];
  const joined = Iterator.concat(tracked(log, 'a'));
  assert.deepStrictEqual(joined.return(), { value: undefined, done: true });
  assert.deepStrictEqual(log, []);
  assert.strictEqual(joined.next().done, true);
}

{
  const log = [];
  const joined = Iterator.concat(tracked(log, 'a'), tracked(log, 'b'));
  joined.next();
  joined.return();
  joined.return();
  assert.deepStrictEqual(log, ['a:open', 'a:next', 'a:return']);
  assert.strictEqual(joined.next().done, true);
}

{
  const log = [];
  for (const value of Iterator.concat(tracked(log, 'a'), tracked(log, 'b'))) break;
  assert.deepStrictEqual(log, ['a:open', 'a:next', 'a:return']);
}

{
  const joined = Iterator.concat(tracked([], 'a', 2, { noReturn: true }));
  joined.next();
  assert.deepStrictEqual(joined.return(), { value: undefined, done: true });
}

{
  const log = [];
  const joined = Iterator.concat(tracked(log, 'a', 2, { throwAt: 1 }), tracked(log, 'b'));
  joined.next();
  assert.throws(() => joined.next(), RangeError);
  assert.strictEqual(joined.next().done, true);
  assert.deepStrictEqual(log, ['a:open', 'a:next', 'a:next', 'a:return']);
}

{
  const log = [];
  const joined = Iterator.concat(tracked(log, 'a', 2, { badStep: true }));
  assert.throws(() => joined.next(), TypeError);
  assert.strictEqual(joined.next().done, true);
}

{
  const joined = Iterator.concat({ [Symbol.iterator]() { return 5; } });
  assert.throws(() => joined.next(), TypeError);
  assert.strictEqual(joined.next().done, true);
}

{
  const joined = Iterator.concat([1], { [Symbol.iterator]() { throw new EvalError('open'); } }, [2]);
  assert.strictEqual(joined.next().value, 1);
  assert.throws(() => joined.next(), EvalError);
  assert.strictEqual(joined.next().done, true);
}

{
  const shared = [1, 2];
  assert.deepStrictEqual([...Iterator.concat(shared, shared)], [1, 2, 1, 2]);
  const once = [1, 2].values();
  assert.deepStrictEqual([...Iterator.concat(once, once)], [1, 2]);
  assert.deepStrictEqual(Iterator.concat([1, 2], [3, 4]).map((x) => x * 2).filter((x) => x > 2).toArray(), [4, 6, 8]);
  assert.deepStrictEqual([...Iterator.concat(Iterator.concat([1], [2]), Iterator.concat(), [3])], [1, 2, 3]);
}

{
  const parts = [];
  for (let i = 0; i < 2000; i++) parts.push([i]);
  let sum = 0;
  for (const value of Iterator.concat(...parts)) sum += value;
  assert.strictEqual(sum, 1999000);
}

console.log('ok');
