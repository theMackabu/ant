const assert = require('node:assert');

function restTail(n, ...rest) {
  if (n === 0) return rest.join(',');
  return restTail(n - 1, 111, 222, 333);
}
function maximumTail(n, ...rest) {
  if (n === 0) return rest.join(',');
  return maximumTail(n - 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
}
function omittedTail(n, value) {
  if (n === 0) return value;
  return omittedTail(n - 1);
}
function noParameters() {
  'use strict';
  if (arguments.length === 0) return noParameters(111, 222, 333);
  return arguments[2];
}

for (let i = 0; i < 400; i++) {
  assert.strictEqual(restTail(2, 111, 222, 333), '111,222,333', `rest at ${i}`);
  assert.strictEqual(maximumTail(2), '1,2,3,4,5,6,7,8,9,10,11,12,13,14,15', `capacity at ${i}`);
  assert.strictEqual(omittedTail(2, 42), undefined, `omitted at ${i}`);
  assert.strictEqual(noParameters(), 333, `zero parameters at ${i}`);
}
console.log('jit self tail rest arguments: ok');
