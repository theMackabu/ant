'use strict';
const assert = require('node:assert');

{
  let dead = {};
  for (let i = 0; i < 100000; i++) dead = new Proxy(dead, {});
  assert.throws(() => dead(), TypeError);
  let live = function () { return 1; };
  for (let i = 0; i < 100000; i++) live = new Proxy(live, {});
  assert.strictEqual(live(), 1);
}

console.log('ok');
