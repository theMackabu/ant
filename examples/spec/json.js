import { test, testDeep, summary } from './helpers.js';

console.log('JSON Tests\n');

let obj1 = JSON.parse('{"name":"John","age":30}');
test('parse object name', obj1.name, 'John');
test('parse object age', obj1.age, 30);

let arr = JSON.parse('[1,2,3,4,5]');
test('parse array length', arr.length, 5);
test('parse array first', arr[0], 1);
test('parse array last', arr[4], 5);

let nested = JSON.parse('{"user":{"name":"Alice","age":25},"active":true}');
test('parse nested name', nested.user.name, 'Alice');
test('parse nested age', nested.user.age, 25);
test('parse nested bool', nested.active, true);

let arrObj = JSON.parse('[{"id":1,"name":"Item1"},{"id":2,"name":"Item2"}]');
test('parse array of objects length', arrObj.length, 2);
test('parse array of objects first name', arrObj[0].name, 'Item1');
test('parse array of objects second id', arrObj[1].id, 2);

let types = JSON.parse('{"active":true,"inactive":false,"data":null,"count":123}');
test('parse bool true', types.active, true);
test('parse bool false', types.inactive, false);
test('parse null', types.data, null);
test('parse number', types.count, 123);

let nums = JSON.parse('{"int":42,"float":3.14,"negative":-10,"zero":0}');
test('parse int', nums.int, 42);
test('parse float', nums.float, 3.14);
test('parse negative', nums.negative, -10);
test('parse zero', nums.zero, 0);

let emptyObj = JSON.parse('{}');
let emptyArr = JSON.parse('[]');
testDeep('parse empty object', emptyObj, {});
test('parse empty array length', emptyArr.length, 0);

let stringified = JSON.stringify([10, 20, 30]);
test('stringify array', stringified, '[10,20,30]');

let objStr = JSON.stringify({ name: 'Charlie', age: 35 });
test('stringify object contains name', objStr.includes('"name"'), true);
test('stringify object contains age', objStr.includes('"age"'), true);

let withTypes = JSON.stringify({ active: true, inactive: false, data: null });
test('stringify bool true', withTypes.includes('true'), true);
test('stringify bool false', withTypes.includes('false'), true);
test('stringify null', withTypes.includes('null'), true);

let original = '{"count":5}';
let parsed = JSON.parse(original);
parsed.count = 10;
test('modify after parse', parsed.count, 10);

let spacedJson = '  {  "key"  :  "value"  }  ';
let spacedParsed = JSON.parse(spacedJson);
test('parse with whitespace', spacedParsed.key, 'value');

let api = JSON.parse('{"status":"success","data":{"user":"john","token":"abc123"}}');
test('parse api status', api.status, 'success');
test('parse api data user', api.data.user, 'john');
test('parse api data token', api.data.token, 'abc123');

let roundtrip = { a: 1, b: 'two', c: true };
let rt = JSON.parse(JSON.stringify(roundtrip));
test('roundtrip a', rt.a, 1);
test('roundtrip b', rt.b, 'two');
test('roundtrip c', rt.c, true);

summary();
