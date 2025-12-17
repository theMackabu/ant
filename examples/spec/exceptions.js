import { test, testDeep, summary } from './helpers.js';

console.log('Exception Tests\n');

let result1 = 'not caught';
try {
  throw new Error('test error');
  result1 = 'should not reach';
} catch (e) {
  result1 = 'caught: ' + e.message;
}
test('basic try-catch', result1, 'caught: test error');

let result2 = 'initial';
try {
  result2 = 'no error';
} catch (e) {
  result2 = 'caught';
}
test('try without exception', result2, 'no error');

let result3 = [];
try {
  result3.push('try');
  throw new Error('error');
} catch (e) {
  result3.push('catch');
} finally {
  result3.push('finally');
}
testDeep('try-catch-finally', result3, ['try', 'catch', 'finally']);

let result4 = [];
try {
  result4.push('try');
} finally {
  result4.push('finally');
}
testDeep('try-finally no error', result4, ['try', 'finally']);

let result5 = 'not caught';
try {
  throw 'error string';
} catch {
  result5 = 'caught without binding';
}
test('catch without binding', result5, 'caught without binding');

let result6 = [];
try {
  result6.push('outer try');
  try {
    result6.push('inner try');
    throw new Error('inner error');
  } catch (e) {
    result6.push('inner catch');
  }
  result6.push('after inner');
} catch (e) {
  result6.push('outer catch');
}
testDeep('nested try-catch', result6, ['outer try', 'inner try', 'inner catch', 'after inner']);

let result7 = [];
try {
  try {
    throw new Error('original');
  } catch (e) {
    result7.push('first catch');
    throw new Error('rethrown');
  }
} catch (e) {
  result7.push('second catch: ' + e.message);
}
testDeep('rethrow', result7, ['first catch', 'second catch: rethrown']);

function testFinallyReturn() {
  let x = [];
  try {
    x.push('try');
    return x;
  } finally {
    x.push('finally');
  }
}
testDeep('finally with return', testFinallyReturn(), ['try', 'finally']);

try {
  throw new Error('test message');
} catch (e) {
  test('error name', e.name, 'Error');
  test('error message', e.message, 'test message');
}

class CustomError extends Error {
  constructor(msg) {
    super(msg);
    this.name = 'CustomError';
  }
}

try {
  throw new CustomError('custom message');
} catch (e) {
  test('custom error name', e.name, 'CustomError');
  test('custom error message', e.message, 'custom message');
}

let counter = 0;
try {
  counter = 1;
  throw new Error('test');
} catch (e) {
  counter = 2;
} finally {
  counter = counter + 10;
}
test('finally modifies value', counter, 12);

summary();
