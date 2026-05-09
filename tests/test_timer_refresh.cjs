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

let postFireRefreshFired = 0;
const postFireRefresh = setTimeout(() => {
  postFireRefreshFired++;
}, 5);

const postFireArgs = [];
const postFireWithArgs = setTimeout((value) => {
  postFireArgs.push(value);
}, 5, 'kept-arg');

setTimeout(() => {
  assert.strictEqual(postFireRefreshFired, 1);
  postFireRefresh.refresh();
  assert.deepStrictEqual(postFireArgs, ['kept-arg']);
  postFireWithArgs.refresh();
}, 20);

setTimeout(() => {
  assert.strictEqual(selfRefreshFired, 2);
  assert.strictEqual(postFireRefreshFired, 2);
  assert.deepStrictEqual(postFireArgs, ['kept-arg', 'kept-arg']);
  clearTimeout(postFireRefresh);
  clearTimeout(postFireWithArgs);
  console.log('timer:refresh:ok');
}, 80);
