const assert = require('node:assert');

let fired = 0;
const timeout = setTimeout(() => {
  fired++;
}, 25);

assert.strictEqual(typeof timeout.refresh, 'function');
assert.strictEqual(timeout.refresh(), timeout);

setTimeout(() => {
  assert.strictEqual(fired, 0);
  timeout.refresh();
}, 10);

setTimeout(() => {
  assert.strictEqual(fired, 1);

  const interval = setInterval(() => {}, 100);
  assert.strictEqual(typeof interval.refresh, 'function');
  assert.strictEqual(interval.refresh(), interval);
  clearInterval(interval);
}, 80);

let selfRefreshFired = 0;
const selfRefreshing = setTimeout(() => {
  selfRefreshFired++;
  if (selfRefreshFired === 1) selfRefreshing.refresh();
}, 5);

setTimeout(() => {
  assert.strictEqual(selfRefreshFired, 2);
  console.log('timer:refresh:ok');
}, 80);
