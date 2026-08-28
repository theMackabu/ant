import assert from 'node:assert';

function completedSyncIterable() {
  return {
    [Symbol.iterator]() {
      return {
        next() {
          return { done: true };
        },
      };
    },
  };
}

function catchAfterCompletedForOf() {
  const marker = new Error('after completed for-of');
  try {
    for (const _value of completedSyncIterable()) {}
    throw marker;
  } catch (error) {
    return error;
  }
}

async function catchAfterCompletedForAwaitOf() {
  const controller = new AbortController();
  controller.abort();

  const iterable = {
    [Symbol.asyncIterator]() {
      return {
        async next() {
          return { done: true };
        },
      };
    },
  };

  try {
    for await (const _value of iterable) {}
    controller.signal.throwIfAborted();
  } catch (error) {
    return error;
  }
}

function catchAfterBrokenForOf() {
  const marker = new Error('after broken for-of');
  let returnCalls = 0;
  const iterable = {
    [Symbol.iterator]() {
      return {
        next() {
          return { done: false, value: 1 };
        },
        return() {
          returnCalls++;
          return { done: true };
        },
      };
    },
  };

  try {
    for (const _value of iterable) break;
    throw marker;
  } catch (error) {
    return { error, returnCalls };
  }
}

async function catchAfterBrokenForAwaitOf() {
  const marker = new Error('after broken for-await-of');
  let returnCalls = 0;
  const iterable = {
    [Symbol.asyncIterator]() {
      return {
        async next() {
          return { done: false, value: 1 };
        },
        async return() {
          returnCalls++;
          return { done: true };
        },
      };
    },
  };

  try {
    for await (const _value of iterable) break;
    throw marker;
  } catch (error) {
    return { error, returnCalls };
  }
}

assert.strictEqual(
  catchAfterCompletedForOf().message,
  'after completed for-of',
);

const abortError = await catchAfterCompletedForAwaitOf();
assert(abortError instanceof Error);
assert.strictEqual(abortError.name, 'AbortError');

const syncBreak = catchAfterBrokenForOf();
assert.strictEqual(syncBreak.error.message, 'after broken for-of');
assert.strictEqual(syncBreak.returnCalls, 1);

const asyncBreak = await catchAfterBrokenForAwaitOf();
assert.strictEqual(asyncBreak.error.message, 'after broken for-await-of');
assert.strictEqual(asyncBreak.returnCalls, 1);

console.log('for-of completion preserves surrounding try handlers');
