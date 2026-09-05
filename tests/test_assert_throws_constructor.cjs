const assert = require('node:assert');
assert.throws(() => { throw new TypeError('test'); }, TypeError);
assert.throws(() => { throw new TypeError('test'); }, Error);
class CustomError extends Error {}
assert.throws(() => { throw new CustomError('test'); }, CustomError);
assert.throws(() => assert.throws(() => { throw new RangeError('wrong'); }, TypeError));
assert.throws(() => assert.throws(() => { throw new Error('wrong'); }, CustomError));
const sentinel = new Error('sentinel');
assert.throws(() => { throw sentinel; }, error => error === sentinel);
assert.throws(() => { throw sentinel; }, function(error) { 'use strict'; return this !== undefined && error === sentinel; });
assert.throws(() => assert.throws(() => { throw sentinel; }, () => false));
assert.throws(() => assert.throws(() => { throw sentinel; }, () => { throw sentinel; }), error => error === sentinel);
assert.throws(() => assert.throws(() => {}, Error));

const errorDescriptor = Object.getOwnPropertyDescriptor(globalThis, 'Error');
let errorReads = 0;
function checkMatchingException() {
  assert.throws(() => { throw 123; }, value => value === 123);
  assert.throws(() => { throw new CustomError('test'); }, CustomError);
}
try {
  globalThis.Error = function ReplacementError() {};
  checkMatchingException();
  assert.throws(
    () => assert.throws(() => { throw sentinel; }, CustomError),
    error => error.message.includes('instance')
  );
  Object.defineProperty(globalThis, 'Error', {
    configurable: true,
    get() { errorReads++; throw sentinel; }
  });
  checkMatchingException();
} finally {
  Object.defineProperty(globalThis, 'Error', errorDescriptor);
}
assert.strictEqual(errorReads, 0);
console.log('assert.throws constructors and validation callbacks ok');
