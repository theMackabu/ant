'use strict';
const assert = require('node:assert');

{
  assert.deepStrictEqual([0].concat(new Proxy(new Proxy([1, 2], {}), {})), [0, 1, 2]);
}

console.log('ok');
