const assert = require('node:assert');

function source(log, options = {}) {
  let index = 0;
  return {
    [Symbol.asyncIterator]() { return this; },
    next() {
      log.push('next');
      if (options.throwOnNext) throw new RangeError('next');
      if (options.rejectOnNext) return Promise.reject(new RangeError('next'));
      const step = { value: index++, done: index > (options.length ?? 5) };
      return options.sync ? step : Promise.resolve(step);
    },
    return() {
      log.push('return');
      if (options.throwOnReturn) throw new SyntaxError('return');
      if (options.rejectOnReturn) return Promise.reject(new SyntaxError('return'));
      return options.sync ? {} : Promise.resolve({});
    },
  };
}

const helpers = {
  map: (s) => AsyncIterator.prototype.map.call(s, (x) => x),
  filter: (s) => AsyncIterator.prototype.filter.call(s, () => true),
  take: (s) => AsyncIterator.prototype.take.call(s, 3),
  drop: (s) => AsyncIterator.prototype.drop.call(s, 1),
  flatMap: (s) => AsyncIterator.prototype.flatMap.call(s, (x) => [x, x]),
};

const done = { value: undefined, done: true };

async function main() {
  for (const sync of [false, true]) {
    for (const [name, make] of Object.entries(helpers)) {
      const label = `${name} ${sync ? 'sync' : 'async'}`;
      {
        const helper = make(source([], { sync }));
        assert.strictEqual(Object.prototype.toString.call(helper), '[object Async Iterator Helper]', label);
        assert.strictEqual(Object.getPrototypeOf(Object.getPrototypeOf(helper)), AsyncIterator.prototype, label);
      }
      {
        const log = [];
        const helper = make(source(log, { sync }));
        assert.deepStrictEqual(await helper.return(), done, label);
        assert.deepStrictEqual(log, ['return'], label);
        assert.deepStrictEqual(await helper.next(), done, label);
        assert.deepStrictEqual(await helper.return(), done, label);
        assert.deepStrictEqual(log, ['return'], label);
      }
      {
        const log = [];
        const helper = make(source(log, { sync }));
        await helper.next();
        await helper.return();
        await helper.return();
        assert.strictEqual(log.filter((entry) => entry === 'return').length, 1, label);
        assert.deepStrictEqual(await helper.next(), done, label);
      }
      {
        const log = [];
        const helper = make(source(log, { sync, length: 2 }));
        while (!(await helper.next()).done);
        const before = log.length;
        assert.deepStrictEqual(await helper.next(), done, label);
        assert.deepStrictEqual(await helper.return(), done, label);
        assert.strictEqual(log.length, before, label);
      }
      {
        const helper = make(source([], { sync, throwOnReturn: sync, rejectOnReturn: !sync }));
        await helper.next();
        await assert.rejects(helper.return(), SyntaxError, label);
        assert.deepStrictEqual(await helper.next(), done, label);
      }
      {
        const log = [];
        const helper = make(source(log, { throwOnNext: sync, rejectOnNext: !sync }));
        await assert.rejects(helper.next(), RangeError, label);
        assert.deepStrictEqual(log, ['next'], label);
        assert.deepStrictEqual(await helper.next(), done, label);
        assert.deepStrictEqual(log, ['next'], label);
      }
      {
        const log = [];
        for await (const value of make(source(log, { sync }))) break;
        assert.strictEqual(log.filter((entry) => entry === 'return').length, 1, label);
      }
    }

    {
      const log = [];
      const helper = AsyncIterator.prototype.take.call(source(log, { sync }), 2);
      assert.deepStrictEqual([(await helper.next()).value, (await helper.next()).value, (await helper.next()).done], [0, 1, true]);
      assert.deepStrictEqual(log, ['next', 'next', 'return']);
      assert.deepStrictEqual(await helper.next(), done);
      assert.deepStrictEqual(log, ['next', 'next', 'return']);
    }
    {
      const helper = AsyncIterator.prototype.take.call(source([], { sync, throwOnReturn: sync, rejectOnReturn: !sync }), 0);
      await assert.rejects(helper.next(), SyntaxError);
      assert.deepStrictEqual(await helper.next(), done);
    }

    for (const name of ['map', 'filter', 'flatMap']) {
      for (const fail of [() => { throw new RangeError('callback'); }, () => Promise.reject(new RangeError('callback'))]) {
        for (const badReturn of [false, true]) {
          const log = [];
          const helper = AsyncIterator.prototype[name].call(source(log, { sync, throwOnReturn: badReturn && sync, rejectOnReturn: badReturn && !sync }), fail);
          await assert.rejects(helper.next(), RangeError, name);
          assert.deepStrictEqual(log, ['next', 'return'], name);
          assert.deepStrictEqual(await helper.next(), done, name);
          assert.deepStrictEqual(await helper.return(), done, name);
          assert.deepStrictEqual(log, ['next', 'return'], name);
        }
      }
    }

    for (const inner of [
      { next() { throw new RangeError('inner'); }, [Symbol.asyncIterator]() { return this; } },
      { next() { return Promise.reject(new RangeError('inner')); }, [Symbol.asyncIterator]() { return this; } },
      { next() { return { get done() { throw new RangeError('inner'); } }; }, [Symbol.asyncIterator]() { return this; } },
      { [Symbol.asyncIterator]() { throw new RangeError('inner'); } },
    ]) {
      const log = [];
      const helper = AsyncIterator.prototype.flatMap.call(source(log, { sync }), () => inner);
      await assert.rejects(helper.next(), RangeError);
      assert.deepStrictEqual(log, ['next', 'return']);
      assert.deepStrictEqual(await helper.next(), done);
    }

    for (const innerFails of [false, true]) {
      const log = [];
      const inner = {
        [Symbol.asyncIterator]() { return this; },
        next() { return { value: 1, done: false }; },
        return() {
          log.push('inner');
          if (innerFails) return sync ? (() => { throw new RangeError('inner'); })() : Promise.reject(new RangeError('inner'));
          return sync ? {} : Promise.resolve({});
        },
      };
      const outer = source(log, { sync, throwOnReturn: innerFails && sync, rejectOnReturn: innerFails && !sync });
      const helper = AsyncIterator.prototype.flatMap.call(outer, () => inner);
      await helper.next();
      if (innerFails) await assert.rejects(helper.return(), RangeError);
      else assert.deepStrictEqual(await helper.return(), done);
      assert.deepStrictEqual(log, ['next', 'inner', 'return']);
      assert.deepStrictEqual(await helper.next(), done);
    }
  }

  {
    const generator = (async function* () { yield 1; })();
    assert.strictEqual(AsyncIterator.from(generator), generator);

    const helper = AsyncIterator.prototype.map.call(source([]), (x) => x);
    assert.strictEqual(AsyncIterator.from(helper), helper);

    const log = [];
    const closed = { closed: true };
    const base = source(log);
    base.return = () => { log.push('return'); return Promise.resolve(closed); };
    const wrapped = AsyncIterator.from(base);
    assert.notStrictEqual(wrapped, base);
    assert.strictEqual(Object.prototype.toString.call(wrapped), '[object AsyncIterator]');
    assert.deepStrictEqual(Reflect.ownKeys(Object.getPrototypeOf(wrapped)), ['next', 'return', 'throw']);
    assert.strictEqual(AsyncIterator.from(wrapped), wrapped);
    await wrapped.next();
    assert.strictEqual(await wrapped.return(), closed);
    assert.strictEqual(await wrapped.return(), closed);
    assert.strictEqual((await wrapped.next()).done, false);
    assert.deepStrictEqual(log, ['next', 'return', 'return', 'next']);
  }

  console.log('ok');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
