import { test, summary } from './helpers.js';

console.log('Typeof Tests\n');

test('typeof number', typeof 42, 'number');
test('typeof float', typeof 3.14, 'number');
test('typeof NaN', typeof NaN, 'number');
test('typeof Infinity', typeof Infinity, 'number');
test('typeof bigint', typeof 123n, 'bigint');

test('typeof string', typeof 'hello', 'string');
test('typeof empty string', typeof '', 'string');
test('typeof template', typeof `template`, 'string');

test('typeof boolean true', typeof true, 'boolean');
test('typeof boolean false', typeof false, 'boolean');

test('typeof undefined', typeof undefined, 'undefined');
test('typeof undeclared', typeof undeclaredVar, 'undefined');

test('typeof null', typeof null, 'object');

test('typeof object', typeof {}, 'object');
test('typeof array', typeof [], 'object');
test('typeof date', typeof new Date(), 'object');
test('typeof regexp', typeof /test/, 'object');

test('typeof function', typeof function () {}, 'function');
test('typeof arrow', typeof (() => {}), 'function');
test('typeof class', typeof class {}, 'function');
test('typeof symbol', typeof Symbol('test'), 'symbol');

summary();
