const assert = require('node:assert');

const calls = [];
function level3() { calls.push(3); throw 'error from level3'; }
function level2() { calls.push(2); level3(); }
function level1() { calls.push(1); level2(); }

let reached = false;
assert.throws(() => { level1(); reached = true; }, (thrown) => thrown === 'error from level3');
assert.deepStrictEqual(calls, [1, 2, 3]);
assert.strictEqual(reached, false);

function errorLevel2() { throw new Error('error from level2'); }
function errorLevel1() { errorLevel2(); }
try {
  errorLevel1();
  assert.fail('expected a throw');
} catch (error) {
  const frames = error.stack.split('\n').slice(1);
  assert.match(frames[0], /^ {4}at errorLevel2 /);
  assert.match(frames[1], /^ {4}at errorLevel1 /);
}

console.log('throw stack ok');
