import { test, summary } from './helpers.js';

console.log('Timer Tests\n');

let results = {};

setTimeout(() => {
  results.timeout = true;
}, 10);

let count = 0;
const interval = setInterval(() => {
  count++;
  if (count >= 3) {
    clearInterval(interval);
    results.interval = count;
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

setTimeout(() => {
  test('setTimeout fired', results.timeout, true);
  test('setInterval count', results.interval, 3);
  test('clearTimeout worked', results.canceled, undefined);
  test('setImmediate fired', results.immediate, true);
  test('queueMicrotask fired', results.microtask, true);
  summary();
}, 100);
