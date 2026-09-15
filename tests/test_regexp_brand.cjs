'use strict';
const assert = require('node:assert');
const exec = RegExp.prototype.exec;
for (const fake of [{}, Object.create(RegExp.prototype), new Proxy(/a/, {})]) {
  assert.throws(() => exec.call(fake, 'a'), TypeError);
}
class DerivedRegExp extends RegExp {}
const values = [/a/g, new RegExp('a', 'g'), new DerivedRegExp('a', 'g')];
for (let i = 0; i < 1000; i++) {
  for (const rx of values) {
    rx.lastIndex = 0;
    const match = exec.call(rx, 'ba');
    assert.strictEqual(match[0], 'a');
    assert.strictEqual(match.index, 1);
    assert.strictEqual(rx.lastIndex, 2);
    assert.strictEqual('ba'.replace(rx, ''), 'b');
  }
}
const recompiled = /a/g;
recompiled.compile('b', 'i');
assert.strictEqual(exec.call(recompiled, 'B')[0], 'B');
assert.strictEqual(exec.call(recompiled, 'a'), null);
// Public properties and prototype changes do not create or remove internal slots.
const real = /c/;
Object.setPrototypeOf(real, null);
assert.strictEqual(exec.call(real, 'c')[0], 'c');
for (let i = 0; i < 10000; i++) {
  const fresh = /x/g;
  assert.strictEqual(exec.call(fresh, 'x')[0], 'x');
  assert.throws(() => exec.call({}, 'x'), TypeError);
}
console.log('PASS RegExp brands, literals, subclasses, compile and ordinary objects');
