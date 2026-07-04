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

function testCatchFinallyReturn(x) {
  try {
    return 'try';
  } catch (e) {
    return 'catch';
  } finally {
    x.push('finally');
  }
}
{
  let x = [];
  test('return in try runs finally (with catch)', testCatchFinallyReturn(x), 'try');
  testDeep('finally ran on return-in-try', x, ['finally']);
}

function testThrowInCatchRunsFinally() {
  let x = [];
  try {
    try {
      throw new Error('a');
    } catch (e) {
      throw new Error('b');
    } finally {
      x.push('finally');
    }
  } catch (e) {
    x.push('outer: ' + e.message);
  }
  return x;
}
testDeep('throw in catch runs finally then propagates', testThrowInCatchRunsFinally(), ['finally', 'outer: b']);

function testNestedFinallyReturn() {
  let x = [];
  try {
    try {
      return x;
    } catch (e) {
      x.push('inner catch');
    } finally {
      x.push('inner finally');
    }
  } catch (e) {
    x.push('outer catch');
  } finally {
    x.push('outer finally');
  }
}
testDeep('return unwinds through nested finallies', testNestedFinallyReturn(), ['inner finally', 'outer finally']);

function testReturnInCatchRunsFinally() {
  let x = [];
  try {
    throw new Error('a');
  } catch (e) {
    x.push('catch');
    return x;
  } finally {
    x.push('finally');
  }
}
testDeep('return in catch runs finally', testReturnInCatchRunsFinally(), ['catch', 'finally']);

function testFinallyOverridesReturn() {
  try {
    return 'try';
  } catch (e) {
    return 'catch';
  } finally {
    return 'finally';
  }
}
test('return in finally overrides return in try', testFinallyOverridesReturn(), 'finally');

{
  let s = '';
  for (let i = 0; i < 3; i++) {
    try {
      if (i === 1) continue;
      if (i === 2) break;
      s += i;
    } catch (e) {} finally { s += 'F'; }
  }
  test('break/continue run finally', s, '0FFF');
}

{
  let s = '';
  outer: for (let i = 0; i < 2; i++) {
    try {
      for (let j = 0; j < 2; j++) {
        try {
          if (j === 1) break outer;
          s += `${i}${j}`;
        } finally { s += 'x'; }
      }
    } finally { s += 'y'; }
  }
  test('labeled break runs nested finallies', s, '00xxy');
}

function testNoStaleHandlerAfterBreak() {
  for (;;) {
    try { break; } catch (e) { return 'stale-catch'; }
  }
  try { throw new Error('after'); } catch (e) { return 'fresh-catch'; }
}
test('break pops try handler', testNoStaleHandlerAfterBreak(), 'fresh-catch');

{
  let s = '';
  for (let i = 0; i < 3; i++) {
    try { s += i; } finally { if (i === 1) break; s += 'F'; }
  }
  test('break in finally discards completion', s, '0F1');
}

summary();
