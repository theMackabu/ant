const assert = require('node:assert');

assert.deepStrictEqual(Object.getOwnPropertyDescriptor(AggregateError.prototype, 'message'), { value: '', writable: true, enumerable: false, configurable: true });
assert.strictEqual(Object.hasOwn(new AggregateError([]), 'message'), false);

console.log('ok');
