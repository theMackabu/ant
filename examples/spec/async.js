import { test, summary } from './helpers.js';

console.log('Async/Await Tests\n');

let results = {};

async function basicAsync() {
  return 42;
}
basicAsync().then(v => { results.basic = v; });

const arrowAsync = async () => 'arrow result';
arrowAsync().then(v => { results.arrow = v; });

const singleParam = async x => x * 2;
singleParam(21).then(v => { results.singleParam = v; });

const multiParam = async (a, b) => a + b;
multiParam(10, 32).then(v => { results.multiParam = v; });

async function withPromise() {
  return Promise.resolve('promise from async');
}
withPromise().then(v => { results.withPromise = v; });

async function conditional(flag) {
  if (flag) return 'true branch';
  return 'false branch';
}
conditional(true).then(v => { results.condTrue = v; });
conditional(false).then(v => { results.condFalse = v; });

const obj = {
  value: 100,
  asyncMethod: async function () {
    return this.value;
  }
};
obj.asyncMethod().then(v => { results.objMethod = v; });

Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => { results.chain = v; });

async function awaiter() {
  const a = await Promise.resolve(1);
  const b = await Promise.resolve(2);
  return a + b;
}
awaiter().then(v => { results.awaiter = v; });

async function awaitValue() {
  return await 42;
}
awaitValue().then(v => { results.awaitValue = v; });

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
  summary();
}, 50);
