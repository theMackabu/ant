const assert = require('node:assert');

function readKey(object, key) { return object[key]; }
const wrapper = new String('xy');
const wrapperChild = { __proto__: wrapper };
const lengths = ['length', 'length'];
for (let i = 0; i < 100000; i++) {
  assert.strictEqual(readKey(String.prototype, lengths[i & 1]), 0);
  assert.strictEqual(readKey(wrapper, lengths[i & 1]), 2);
  assert.strictEqual(readKey(wrapperChild, lengths[i & 1]), 2);
  assert.strictEqual(readKey({ length: 5 }, lengths[i & 1]), 5);
}

console.log('ok');
