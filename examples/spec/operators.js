import { test, summary } from './helpers.js';

console.log('Operator Tests\n');

test('5 == 5', 5 == 5, true);
test('5 === 5', 5 === 5, true);
test('5 != 10', 5 != 10, true);
test('5 !== 10', 5 !== 10, true);
test('5 == 10', 5 == 10, false);
test('5 != 5', 5 != 5, false);

test("'hello' == 'hello'", 'hello' == 'hello', true);
test("'hello' === 'hello'", 'hello' === 'hello', true);
test("'hello' != 'world'", 'hello' != 'world', true);
test("'hello' == 'world'", 'hello' == 'world', false);

test('true == true', true == true, true);
test('true === true', true === true, true);
test('true != false', true != false, true);
test('false == false', false == false, true);

test('undefined == undefined', undefined == undefined, true);
test('undefined === undefined', undefined === undefined, true);
test('null == null', null == null, true);
test('null === null', null === null, true);
test('undefined == null', undefined == null, true);
test('undefined === null', undefined === null, false);

test('5 !== "5"', 5 !== '5', true);
test('true !== 1', true !== 1, true);

let obj1 = { x: 1 };
let obj2 = { x: 1 };
let obj3 = obj1;
test('obj1 == obj2 (diff refs)', obj1 == obj2, false);
test('obj1 === obj3 (same ref)', obj1 === obj3, true);

let arr1 = [1, 2, 3];
let arr2 = [1, 2, 3];
let arr3 = arr1;
test('arr1 == arr2 (diff refs)', arr1 == arr2, false);
test('arr1 === arr3 (same ref)', arr1 === arr3, true);

test('5 > 3', 5 > 3, true);
test('3 < 5', 3 < 5, true);
test('5 >= 5', 5 >= 5, true);
test('5 <= 5', 5 <= 5, true);
test('5 > 5', 5 > 5, false);
test('5 < 5', 5 < 5, false);

test('2 + 3', 2 + 3, 5);
test('5 - 3', 5 - 3, 2);
test('4 * 3', 4 * 3, 12);
test('12 / 4', 12 / 4, 3);
test('10 % 3', 10 % 3, 1);
test('2 ** 3', 2 ** 3, 8);

test('true && true', true && true, true);
test('true && false', true && false, false);
test('true || false', true || false, true);
test('false || false', false || false, false);
test('!true', !true, false);
test('!false', !false, true);

test('5 & 3', 5 & 3, 1);
test('5 | 3', 5 | 3, 7);
test('5 ^ 3', 5 ^ 3, 6);
test('~5', ~5, -6);
test('5 << 1', 5 << 1, 10);
test('5 >> 1', 5 >> 1, 2);
test('-5 >>> 1', (-5 >>> 1) > 0, true);

test('typeof 5', typeof 5, 'number');
test("typeof 'hello'", typeof 'hello', 'string');
test('typeof true', typeof true, 'boolean');
test('typeof undefined', typeof undefined, 'undefined');
test('typeof null', typeof null, 'object');
test('typeof {}', typeof {}, 'object');
test('typeof []', typeof [], 'object');
test('typeof function(){}', typeof function(){}, 'function');

test("'a' in {a:1}", 'a' in {a:1}, true);
test("'b' in {a:1}", 'b' in {a:1}, false);

test('[] instanceof Array', [] instanceof Array, true);
test('{} instanceof Object', {} instanceof Object, true);

test('ternary true', true ? 'yes' : 'no', 'yes');
test('ternary false', false ? 'yes' : 'no', 'no');

test('nullish ?? default', null ?? 'default', 'default');
test('nullish defined', 'value' ?? 'default', 'value');
test('nullish 0', 0 ?? 'default', 0);

test('optional ?.', { a: { b: 1 } }?.a?.b, 1);
test('optional ?. undefined', { a: {} }?.a?.b, undefined);
test('optional ?. null chain', null?.a?.b, undefined);

let x = 5;
test('++x prefix', ++x, 6);
test('x++ postfix', x++, 6);
test('after x++', x, 7);
test('--x prefix', --x, 6);

let y = 10;
y += 5;
test('+= assignment', y, 15);
y -= 3;
test('-= assignment', y, 12);
y *= 2;
test('*= assignment', y, 24);
y /= 4;
test('/= assignment', y, 6);

summary();
