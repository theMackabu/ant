import { test, summary } from './helpers.js';

console.log('Promise Tests\n');

let resolved = false;
let resolvedValue = null;
let p = new Promise((resolve) => resolve(42));
test('Promise instanceof', p instanceof Promise, true);

p.then(v => {
  resolved = true;
  resolvedValue = v;
});

let chainedValue = null;
let p2 = new Promise(resolve => resolve(10));
p2.then(v => v * 2).then(v => { chainedValue = v; });

let caughtError = null;
let p3 = new Promise((_, reject) => reject('error'));
p3.catch(e => { caughtError = e; });

let staticValue = null;
Promise.resolve('static').then(v => { staticValue = v; });

let tryValue = null;
Promise.try(() => 'try').then(v => { tryValue = v; });

let finallyCalled = false;
Promise.resolve('fin').finally(() => { finallyCalled = true; });

setTimeout(() => {
  test('resolve called', resolved, true);
  test('resolved value', resolvedValue, 42);
  test('chained value', chainedValue, 20);
  test('caught error', caughtError, 'error');
  test('static resolve', staticValue, 'static');
  test('Promise.try', tryValue, 'try');
  test('finally called', finallyCalled, true);

  let allResolved = null;
  Promise.all([Promise.resolve(1), Promise.resolve(2), Promise.resolve(3)])
    .then(arr => { allResolved = arr; });

  let raceResolved = null;
  Promise.race([Promise.resolve('first'), Promise.resolve('second')])
    .then(v => { raceResolved = v; });

  let anyResolved = null;
  Promise.any([Promise.reject('fail'), Promise.resolve('success')])
    .then(v => { anyResolved = v; });

  setTimeout(() => {
    test('Promise.all length', allResolved?.length, 3);
    test('Promise.all values', allResolved?.[0] === 1 && allResolved?.[1] === 2, true);
    test('Promise.race first', raceResolved, 'first');
    test('Promise.any success', anyResolved, 'success');
    summary();
  }, 10);
}, 10);
