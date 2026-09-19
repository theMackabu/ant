function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(action, reason, label) {
  let caught = false;
  try { action(); }
  catch (error) {
    caught = true;
    assert(error === reason, label + ': original throw changed');
  }
  assert(caught, label + ': throw was swallowed');
}

function source(reason, getter, value = 1) {
  let closed = 0;
  const iterator = {
    [Symbol.iterator]() { return this; },
    next() { return { value, done: false }; },
  };
  if (getter) {
    Object.defineProperty(iterator, 'return', {
      get() { closed++; throw reason; },
    });
  } else {
    iterator.return = () => { closed++; throw reason; };
  }
  return { iterator, count: () => closed };
}

let queued = false;
queueMicrotask(() => { queued = true; });
for (const reason of [new Error('original'), undefined, null]) {
  for (const closeReason of [new Error('close'), undefined, null]) {
    for (const getter of [false, true]) {
      for (const method of ['every', 'some', 'find', 'forEach', 'reduce']) {
        const state = source(closeReason, getter);
        expectThrow(() => Iterator.prototype[method].call(state.iterator, () => {
          throw reason;
        }, 0), reason, method);
        assert(state.count() === 1, method + ': iterator was not closed once');
      }

      class ThrowingMap extends Map {
        set() { throw reason; }
      }
      const state = source(closeReason, getter, [1, 2]);
      expectThrow(() => new ThrowingMap(state.iterator), reason, 'Map adder');
      assert(state.count() === 1, 'Map iterator was not closed once');
    }
  }
}

for (const closeReason of [new Error('close'), undefined, null]) {
  for (const getter of [false, true]) {
    for (const method of ['every', 'some', 'find']) {
      const state = source(closeReason, getter);
      expectThrow(() => Iterator.prototype[method].call(state.iterator, () => method !== 'every'),
        closeReason, method + ' normal close');
      assert(state.count() === 1, method + ': iterator was not closed once');
    }
    const state = source(closeReason, getter);
    expectThrow(() => new Set().isSupersetOf({
      size: 0, has() { return false; }, keys() { return state.iterator; },
    }), closeReason, 'Set keys normal close');
    assert(state.count() === 1, 'Set keys iterator was not closed once');
  }
}

const invalidEntry = source(new Error('close'), false);
let invalidCaught = false;
try { new Map(invalidEntry.iterator); }
catch (error) { invalidCaught = true; assert(error instanceof TypeError, 'Map validation error was replaced'); }
assert(invalidCaught, 'Map validation error was swallowed');

for (const reason of [new Error('typed array source'), undefined, null]) {
  for (const property of [Symbol.iterator, 'length', '0']) {
    const arrayLike = { length: 1, 0: 1 };
    Object.defineProperty(arrayLike, property, { get() { throw reason; } });
    expectThrow(() => Uint8Array.from(arrayLike), reason, 'TypedArray.from source getter');
  }
}

for (const reason of [new Error('iterator step'), undefined, null]) {
  for (const method of ['every', 'some', 'find', 'forEach', 'reduce', 'toArray']) {
    for (const operation of ['next getter', 'next call', 'done', 'value']) {
      const iterator = {
        [Symbol.iterator]() { return this; },
        next() {
          if (operation === 'next call') throw reason;
          const step = { value: 1, done: false };
          Object.defineProperty(step, operation, { get() { throw reason; } });
          return step;
        },
      };
      if (operation === 'next getter') {
        Object.defineProperty(iterator, 'next', { get() { throw reason; } });
      }
      expectThrow(() => Iterator.prototype[method].call(iterator, () => {
        throw new Error('callback ran after iterator failure');
      }, 0), reason, method + ' ' + operation);
    }
  }
}

for (const reason of [new Error('first inner step'), undefined, null]) {
  for (const operation of ['next', 'done', 'value']) {
    let nextCalls = 0;
    let doneReads = 0;
    let valueReads = 0;
    const inner = {
      [Symbol.iterator]() { return this; },
      get next() {
        if (operation === 'next') throw reason;
        return () => {
          nextCalls++;
          return {
            get done() {
              doneReads++;
              if (operation === 'done') throw reason;
              return false;
            },
            get value() {
              valueReads++;
              throw reason;
            },
          };
        };
      },
    };
    const iterator = [0].values().flatMap(() => inner);
    expectThrow(() => iterator.next(), reason, 'flatMap first inner ' + operation);
    assert(nextCalls === (operation === 'next' ? 0 : 1), 'flatMap called next after getter failure');
    assert(doneReads === (operation === 'next' ? 0 : 1), 'flatMap read done after getter failure');
    assert(valueReads === (operation === 'value' ? 1 : 0), 'flatMap read value after getter failure');
  }
}

const flattened = [0, 1, 2].values().flatMap(value => value === 0 ? [] : [value, value + 10]);
assert(flattened.toArray().join(',') === '1,11,2,12', 'flatMap successful iteration changed');

for (const reason of [new Error('inner next'), undefined, null]) {
  for (const operation of ['next', 'done', 'value']) {
    let calls = 0;
    const inner = {
      [Symbol.iterator]() { return this; },
      next() {
        if (calls++ === 0) return { value: 1, done: false };
        if (operation === 'next') throw reason;
        const step = { value: 2, done: false };
        Object.defineProperty(step, operation, { get() { throw reason; } });
        return step;
      },
    };
    const iterator = [0].values().flatMap(() => inner);
    assert(iterator.next().value === 1, 'flatMap initial result changed');
    expectThrow(() => iterator.next(), reason, 'flatMap inner ' + operation);
  }
}
setTimeout(() => {
  assert(queued, 'queued work was dropped');
  console.log('PASS');
}, 0);
