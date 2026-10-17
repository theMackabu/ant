const assert = require('node:assert');
const vm = require('node:vm');

const ctx = vm.createContext({});
const max = vm.runInContext('Math.max', ctx);
const parseIntOther = vm.runInContext('parseInt', ctx);
const isNaNOther = vm.runInContext('isNaN', ctx);
const OtherTypeError = vm.runInContext('TypeError', ctx);

for (let i = 0; i < 2000; i++) {
  assert.strictEqual(max(i, 7), Math.max(i, 7));
  assert.strictEqual(parseIntOther('ff', 16), 255);
}

assert.strictEqual(max(), -Infinity);
assert.strictEqual(max(1, 'x'), NaN);
assert.strictEqual(max(null, undefined === undefined), 1);
assert.strictEqual(isNaNOther('abc'), true);

for (const bad of [Symbol('s')]) {
  let caught;
  try { max(bad, 1); } catch (e) { caught = e; }
  assert.ok(caught instanceof OtherTypeError, `${typeof bad} error comes from the builtin's realm`);
  assert.ok(!(caught instanceof TypeError), `${typeof bad} error is not from the caller's realm`);
}

let calls = 0;
assert.strictEqual(max({ valueOf() { calls++; return 9; } }, 3), 9);
assert.strictEqual(calls, 1);

let inner;
try { max({ valueOf() { null.x; } }); } catch (e) { inner = e; }
assert.ok(inner instanceof TypeError, 'errors thrown by caller-realm valueOf stay in the caller realm');

try { Math.max(Symbol()); } catch (e) { assert.ok(e instanceof TypeError && !(e instanceof OtherTypeError)); }
console.log('realm-free builtins: ok');
