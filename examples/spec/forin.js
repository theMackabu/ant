import { test, testDeep, summary } from './helpers.js';

console.log('For-In Enumeration Tests\n');

var strPrim = 'abc';
var strPrimKeys = [];
for (var k in strPrim) strPrimKeys.push(k);
testDeep('string primitive for-in keys', strPrimKeys, ['0', '1', '2']);
test('string primitive length', strPrim.length, 3);

var strObj = new String('abc');
var strObjKeys = [];
for (var k in strObj) strObjKeys.push(k);
testDeep('String object for-in keys', strObjKeys, ['0', '1', '2']);
test('String object length', strObj.length, 3);

var sparse = [];
sparse[5] = 'hello';
test('sparse array length', sparse.length, 6);
var sparseKeys = [];
for (var k in sparse) sparseKeys.push(k);
testDeep('sparse array for-in keys', sparseKeys, ['5']);

var arr = [10, 20, 30];
var arrKeys = [];
for (var k in arr) arrKeys.push(k);
testDeep('array for-in keys', arrKeys, ['0', '1', '2']);

var obj = { a: 1, b: 2, c: 3 };
var objKeys = [];
for (var k in obj) objKeys.push(k);
testDeep('object for-in keys', objKeys, ['a', 'b', 'c']);

var arrWithProps = [1, 2];
arrWithProps.foo = 'bar';
var arrPropsKeys = [];
for (var k in arrWithProps) arrPropsKeys.push(k);
testDeep('array with properties for-in keys', arrPropsKeys, ['0', '1', 'foo']);

summary();
