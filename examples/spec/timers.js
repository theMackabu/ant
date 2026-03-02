import { test, summary } from './helpers.js';

console.log('Timer Tests\n');

let results = {};
let intervalDoneResolve;
const intervalDone = new Promise(resolve => {
  intervalDoneResolve = resolve;
});

setTimeout(() => {
  results.timeout = true;
}, 10);

let count = 0;
const interval = setInterval(() => {
  count++;
  if (count >= 3) {
    clearInterval(interval);
    results.interval = count;
    intervalDoneResolve();
  }
}, 10);

const canceled = setTimeout(() => {
  results.canceled = true;
}, 10);
clearTimeout(canceled);

setImmediate(() => {
  results.immediate = true;
});

queueMicrotask(() => {
  results.microtask = true;
});

Promise.all([
  new Promise(resolve => setTimeout(resolve, 10)),
  intervalDone,
]).then(() => {
  test('setTimeout fired', results.timeout, true);
  test('setInterval count', results.interval, 3);
  test('clearTimeout worked', results.canceled, undefined);
  test('setImmediate fired', results.immediate, true);
  test('queueMicrotask fired', results.microtask, true);
  summary();
});
