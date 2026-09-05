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
console.log('assert.throws constructors and validation callbacks ok');
