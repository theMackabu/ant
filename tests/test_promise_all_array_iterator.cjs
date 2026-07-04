const assert = require('assert');

const p = new Promise(resolve => setTimeout(() => resolve(30), 1));

Promise.all([p]).then(values => {
  assert.strictEqual(values[0], 30);
  console.log('promise-all-array-iterator:ok');
});

p.then(value => {
  assert.strictEqual(value, 30);
});
