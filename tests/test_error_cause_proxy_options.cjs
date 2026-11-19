'use strict';
const assert = require('node:assert');

{
  const log = [];
  const options = new Proxy({ cause: 7 }, { has(t, k) { log.push('has ' + String(k)); return k in t; }, get(t, k) { log.push('get ' + String(k)); return t[k]; } });
  assert.strictEqual(new Error('x', options).cause, 7);
  assert.deepStrictEqual(log, ['has cause', 'get cause']);
}

console.log('ok');
