const assert = require('node:assert');
function single(value) { return `${value}`; }
function adjacent(a, b, c) { return `${a}${b}${c}`; }
function append(a, b) { a = `${a}${b}`; return a; }
for (let i = 0; i < 30000; i++) {
  assert.strictEqual(single(i), String(i));
  assert.strictEqual(adjacent('a', i & 1 ? '' : '😀', 'z'), i & 1 ? 'az' : 'a😀z');
  assert.strictEqual(append('left', 'right'), 'leftright');
}
assert.strictEqual(``, '');
assert.strictEqual(single(undefined), 'undefined');
assert.strictEqual(single(null), 'null');
assert.strictEqual(single(123n), '123');
assert.throws(() => single(Symbol('x')), TypeError);
assert.strictEqual(adjacent('', '', ''), '');

const events = [];
function item(name) {
  events.push('eval ' + name);
  return { [Symbol.toPrimitive](hint) { events.push('convert ' + name + ' ' + hint); return name; } };
}
assert.strictEqual(`${item('a')}${item('b')}${item('c')}`, 'abc');
assert.deepStrictEqual(events, ['eval a', 'convert a string', 'eval b', 'convert b string', 'eval c', 'convert c string']);
let reached = false;
assert.throws(() => `${Symbol()}${reached = true}`, TypeError);
assert.strictEqual(reached, false);

function snapshot() {
  let value = 'initial';
  const before = `${value}`;
  value += ' changed';
  assert.strictEqual(before, 'initial');
  value = `${value}${{ toString() { value = 'reentrant'; return '!'; } }}`;
  assert.strictEqual(value, 'initial changed!');
  value = `${value}`;
  assert.strictEqual(value, 'initial changed!');
}
for (let i = 0; i < 30000; i++) snapshot();

// Tagged templates retain all literal segments and do not coerce substitutions.
const marker = Symbol('marker');
function tagged(strings, a, b) {
  assert.deepStrictEqual([...strings], ['', '', '']);
  assert.deepStrictEqual([...strings.raw], ['', '', '']);
  assert.strictEqual(a, marker);
  assert.strictEqual(b, 42);
}
tagged`${marker}${42}`;
console.log('empty template segments, coercion order, snapshots and tags ok');
