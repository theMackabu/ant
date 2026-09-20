function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function poisonedPromise(reason, value) {
  const promise = Promise.resolve(value);
  Object.defineProperty(promise, 'constructor', { get() { throw reason; } });
  return promise;
}

function source(next, close) {
  return {
    [Symbol.asyncIterator]() { return this; },
    next,
    return: close || (() => ({ done: true })),
  };
}

function rejected(promise, reason, label) {
  assert(promise instanceof Promise, label + ': expected a Promise');
  return promise.then(
    () => { throw new Error(label + ': expected rejection'); },
    error => { assert(error === reason, label + ': original reason changed'); }
  );
}

const checks = [];
let queued = false;
queueMicrotask(() => { queued = true; });
const deadline = setTimeout(() => { throw new Error('async iterator error never settled'); }, 3000);

for (const reason of [new Error('promise constructor'), undefined, null]) {
  const step = () => ({ value: 1, done: false });
  checks.push(rejected(AsyncIterator.prototype.map.call(
    source(() => poisonedPromise(reason, step())), value => value
  ).next(), reason, 'wrapper next'));

  checks.push(rejected(AsyncIterator.prototype.flatMap.call(source(step), () => source(
    () => poisonedPromise(reason, step())
  )).next(), reason, 'inner next'));

  checks.push(rejected(AsyncIterator.from([poisonedPromise(reason, 1)]).next(), reason, 'sync iterator value'));

  checks.push(rejected(AsyncIterator.prototype.map.call(source(step), () =>
    poisonedPromise(reason, 1)
  ).next(), reason, 'wrapper callback'));

  checks.push(rejected(AsyncIterator.prototype.toArray.call(
    source(() => poisonedPromise(reason, step()))
  ), reason, 'terminal next'));

  let callbackClosed = 0;
  checks.push(rejected(AsyncIterator.prototype.forEach.call(source(step, () => {
    callbackClosed++;
    return { done: true };
  }), () => poisonedPromise(reason, 1)), reason, 'terminal callback').then(() => {
    assert(callbackClosed === 1, 'failed terminal callback did not close iterator');
  }));

  checks.push(rejected(AsyncIterator.prototype.reduce.call(source(step), () =>
    poisonedPromise(reason, 1), 0
  ), reason, 'reduce callback'));

  checks.push(rejected(AsyncIterator.prototype.some.call(source(step, () =>
    poisonedPromise(reason, { done: true })
  ), () => true), reason, 'normal close'));

  checks.push(rejected(AsyncIterator.prototype.forEach.call(source(step, () =>
    poisonedPromise(reason, { done: true })
  ), () => { throw reason; }), reason, 'abrupt close'));
}

Promise.all(checks).then(() => {
  assert(queued, 'queued work was dropped');
  clearTimeout(deadline);
  console.log('PASS');
}, error => {
  clearTimeout(deadline);
  console.error(error);
  process.exitCode = 1;
});
