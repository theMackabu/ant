import { test, summary } from './helpers.js';

console.log('Async/Await Tests\n');

let results = {};

async function basicAsync() {
  return 42;
}
basicAsync().then(v => {
  results.basic = v;
});

const arrowAsync = async () => 'arrow result';
arrowAsync().then(v => {
  results.arrow = v;
});

const singleParam = async x => x * 2;
singleParam(21).then(v => {
  results.singleParam = v;
});

const multiParam = async (a, b) => a + b;
multiParam(10, 32).then(v => {
  results.multiParam = v;
});

async function withPromise() {
  return Promise.resolve('promise from async');
}
withPromise().then(v => {
  results.withPromise = v;
});

async function conditional(flag) {
  if (flag) return 'true branch';
  return 'false branch';
}
conditional(true).then(v => {
  results.condTrue = v;
});
conditional(false).then(v => {
  results.condFalse = v;
});

const obj = {
  value: 100,
  asyncMethod: async function () {
    return this.value;
  }
};
obj.asyncMethod().then(v => {
  results.objMethod = v;
});

Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => {
    results.chain = v;
  });

async function awaiter() {
  const a = await Promise.resolve(1);
  const b = await Promise.resolve(2);
  return a + b;
}
awaiter().then(v => {
  results.awaiter = v;
});

async function awaitValue() {
  return await 42;
}
awaitValue().then(v => {
  results.awaitValue = v;
});

let finallyCalledResolve = false;
Promise.resolve('original')
  .finally(() => {
    finallyCalledResolve = true;
    return 'ignored';
  })
  .then(v => {
    results.finallyNoChange = v;
  });

let finallyCalledReject = false;
Promise.reject('rejected reason')
  .finally(() => {
    finallyCalledReject = true;
    return 'also ignored';
  })
  .catch(e => {
    results.finallyRejectPass = e;
  });

Promise.reject('foobar')
  .finally(() => {
    throw new Error('bar');
  })
  .catch(e => {
    results.finallyThrowError = e.message;
  });

Promise.reject('foobar')
  .finally(() => {
    return Promise.reject('changed');
  })
  .catch(e => {
    results.finallyRejectChange = e;
  });

async function awaitLoop() {
  let sum = 0;
  for (let i = 0; i < 3; i++) {
    sum += await Promise.resolve(i);
  }
  return sum;
}
awaitLoop().then(v => {
  results.awaitLoop = v;
});

async function inner() {
  return await Promise.resolve(10);
}
async function outer() {
  return (await inner()) + (await inner());
}
outer().then(v => {
  results.nestedAsync = v;
});

async function sequential() {
  const a = await Promise.resolve(1);
  const b = await Promise.resolve(2);
  const c = await Promise.resolve(3);
  return a + b + c;
}
sequential().then(v => {
  results.sequential = v;
});

setTimeout(() => {
  test('basic async', results.basic, 42);
  test('arrow async', results.arrow, 'arrow result');
  test('single param async', results.singleParam, 42);
  test('multi param async', results.multiParam, 42);
  test('with promise', results.withPromise, 'promise from async');
  test('conditional true', results.condTrue, 'true branch');
  test('conditional false', results.condFalse, 'false branch');
  test('object method', results.objMethod, 100);
  test('promise chain', results.chain, 4);
  test('awaiter sum', results.awaiter, 3);
  test('await value', results.awaitValue, 42);
  test('finally no change resolve', results.finallyNoChange, 'original');
  test('finally called on resolve', finallyCalledResolve, true);
  test('finally passes rejection', results.finallyRejectPass, 'rejected reason');
  test('finally called on reject', finallyCalledReject, true);
  test('finally throw changes rejection', results.finallyThrowError, 'bar');
  test('finally reject changes rejection', results.finallyRejectChange, 'changed');
  test('await in loop', results.awaitLoop, 3);
  test('nested async', results.nestedAsync, 20);
  test('sequential awaits', results.sequential, 6);
  summary();
}, 50);
