const assert = require('node:assert');

{
  let chain = { base: 1 };
  for (let i = 0; i < 300; i++) { chain = Object.create(chain); chain['k' + i] = 1; }
  let count = 0;
  for (const key in chain) count++;
  assert.strictEqual(count, 301);
}

console.log('ok');
