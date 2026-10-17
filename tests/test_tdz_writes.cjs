const assert = require('node:assert');

function throwsRef(fn, label) {
  assert.throws(fn, ReferenceError, label);
}

throwsRef(() => { late = 1; let late; }, 'assign');
throwsRef(() => { late += 1; let late; }, 'compound');
throwsRef(() => { late ??= 1; let late; }, 'logical');
throwsRef(() => { late++; let late; }, 'postfix');
throwsRef(() => { [late] = [1]; let late; }, 'array destructure');
throwsRef(() => { ({ late } = { late: 1 }); let late; }, 'object destructure');
throwsRef(() => { for (late in { a: 1 }); let late; }, 'for-in target');
throwsRef(() => { for (late of [1]); let late; }, 'for-of target');
throwsRef(() => { late = 1; const late = 0; }, 'const before init');
throwsRef(() => { K = 1; class K {} }, 'class before init');
throwsRef(() => { const f = () => { late = 1; }; f(); let late; }, 'upvalue assign');
throwsRef(() => { const f = () => { late += 1; }; f(); let late; }, 'upvalue compound');

assert.throws(() => { const c = 0; c = 1; }, TypeError);
assert.strictEqual((() => { let x; x = 5; x += 1; return x; })(), 6);
assert.strictEqual((() => { const f = () => { v = 7; }; let v = 1; f(); return v; })(), 7);

function hotUpvalue(kind) {
  const read = () => late;
  const write = (v) => { late = v; };
  let errors = 0;
  for (let i = 0; i < 5000; i++) {
    try { kind === 'read' ? read() : write(i); } catch (e) {
      assert.ok(e instanceof ReferenceError);
      errors++;
    }
  }
  let late = 1;
  write(9);
  return `${errors}:${read()}`;
}
assert.strictEqual(hotUpvalue('read'), '5000:9');
assert.strictEqual(hotUpvalue('write'), '5000:9');

function* lateBinding(cb) {
  cb((n) => { let s = 0; for (let i = 0; i < n; i++) s += late; return s; });
  yield;
  let late = 2;
  yield;
}
let sum;
const it = lateBinding(f => { sum = f; });
it.next();
for (let r = 0; r < 50; r++) throwsRef(() => sum(2000), 'hoisted loop read before init');
it.next();
for (let r = 0; r < 50; r++) assert.strictEqual(sum(2000), 4000);

console.log('tdz writes: ok');
