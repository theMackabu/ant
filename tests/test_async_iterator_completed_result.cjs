const assert = require('node:assert');

function completedSource(promised) {
  let reads = 0;
  return {
    reads: () => reads,
    [Symbol.asyncIterator]() { return this; },
    next() {
      const step = {
        done: true,
        get value() {
          reads++;
          throw new Error('completed value must not be read');
        },
      };
      return promised ? Promise.resolve(step) : step;
    },
  };
}

async function main() {
  const unused = () => { throw new Error('completed result must not invoke a callback'); };
  for (const promised of [false, true]) {
    for (const [method, args] of [
      ['map', [unused]], ['filter', [unused]], ['flatMap', [unused]],
      ['take', [1]], ['drop', [0]],
    ]) {
      const source = completedSource(promised);
      const helper = AsyncIterator.prototype[method].call(source, ...args);
      assert.deepStrictEqual(await helper.next(), { done: true, value: undefined });
      assert.strictEqual(source.reads(), 0, method);
    }
    const source = completedSource(promised);
    assert.deepStrictEqual(await AsyncIterator.from(source).next(), { done: true, value: undefined });
    assert.strictEqual(source.reads(), 0, 'AsyncIterator.from async source');

    for (const [method, args, expected] of [
      ['toArray', [], []], ['every', [unused], true], ['some', [unused], false],
      ['find', [unused], undefined], ['forEach', [unused], undefined],
      ['reduce', [unused, 42], 42],
    ]) {
      const source = completedSource(promised);
      assert.deepStrictEqual(await AsyncIterator.prototype[method].call(source, ...args), expected);
      assert.strictEqual(source.reads(), 0, method);
    }
    const empty = completedSource(promised);
    await assert.rejects(AsyncIterator.prototype.reduce.call(empty, unused), TypeError);
    assert.strictEqual(empty.reads(), 0, 'empty reduce');
  }

  for (const method of ['next', 'return', 'throw']) {
    for (const promised of [false, true]) {
      let reads = 0;
      const source = {
        [Symbol.iterator]() { return this; },
        next() { return { done: true }; },
        [method]() {
          return { done: true, get value() { reads++; return promised ? Promise.resolve(42) : 42; } };
        },
      };
      assert.deepStrictEqual(await AsyncIterator.from(source)[method](), { done: true, value: 42 });
      assert.strictEqual(reads, 1, 'sync ' + method);
    }
    const reason = new Error('sync completed value');
    for (const rejected of [false, true]) {
      const source = {
        next() { return { done: true }; },
        [method]() {
          return {
            done: true,
            get value() {
              if (rejected) return Promise.reject(reason);
              throw reason;
            },
          };
        },
      };
      await assert.rejects(AsyncIterator.from(source)[method](), error => error === reason);
    }
  }
}

const deadline = setTimeout(() => process.exit(1), 3000);
main().then(() => {
  clearTimeout(deadline);
  console.log('Async iterator completed results ok');
}, error => {
  console.error(error);
  process.exit(1);
});
