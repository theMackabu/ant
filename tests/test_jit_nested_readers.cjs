'use strict';
const assert = require('node:assert');

if (process.argv[2] === 'child' || typeof Ant === 'undefined') {
  function Reader() { this.value = 7; this.calls = 0; }
  Reader.prototype.read = function readLeaf() { return this.value; };
  Reader.prototype.wrap = function wrapReader() { return this.read(); };
  Reader.prototype.withEffect = function effectReader() {
    this.calls = this.calls + 1;
    return this.read();
  };
  function readerDriver(reader, count) {
    let sum = 0;
    for (let i = 0; i < count; i++) sum += reader.wrap();
    return sum;
  }
  function effectDriver(reader, count) {
    let sum = 0;
    for (let i = 0; i < count; i++) sum += reader.withEffect();
    return sum;
  }
  const reader = new Reader();
  for (let i = 0; i < 150; i++) { reader.read(); reader.wrap(); reader.withEffect(); }
  assert.strictEqual(readerDriver(reader, 2000), 14000);
  assert.strictEqual(effectDriver(reader, 2000), 14000);
  const calls = reader.calls;
  let gets = 0;
  Object.defineProperty(reader, 'value', { configurable: true, get() { gets++; return 11; } });
  assert.strictEqual(effectDriver(reader, 300), 3300);
  assert.strictEqual(reader.calls, calls + 300, 'inner fallback must not replay outer writes');
  assert.strictEqual(gets, 300, 'getter must run once');
  reader.read = function replacement() { return 19; };
  assert.strictEqual(readerDriver(reader, 300), 5700);
  reader.read = Reader.prototype.read.bind({ value: 23 });
  assert.strictEqual(readerDriver(reader, 300), 6900);
  reader.read = Reader.prototype.read.bind({ value: 31 }, 99);
  assert.strictEqual(readerDriver(reader, 300), 9300);
  const error = new Error('reader failure');
  reader.read = function throwing() { throw error; };
  const beforeThrow = reader.calls;
  assert.throws(() => effectDriver(reader, 1), value => value === error);
  assert.strictEqual(reader.calls, beforeThrow + 1);

  const collection = {
    items: [3, 5, 7],
    at(index) { return this.items[index]; },
    twice(index) { const first = this.at(index); return first === this.at(index); },
  };
  for (let i = 0; i < 200; i++) { collection.at(i % 3); collection.twice(i % 3); }
  function collectionDriver(object, count) {
    for (let i = 0; i < count; i++) assert.strictEqual(object.twice(i % 3), true);
  }
  collectionDriver(collection, 1000);
  let indexedGets = 0;
  Object.defineProperty(collection.items, '0', { get() { indexedGets++; return 3; } });
  collectionDriver(collection, 300);
  assert.strictEqual(indexedGets, 200, 'nested calls must preserve the original index');
  function makeSelector(initial) {
    let selected = initial;
    return { read: function selectLeaf() { return this.value == selected ? this.left : this.right; },
      set(value) { selected = value; } };
  }
  function selectWrapper() { return this.choose(); }
  function selectorDriver(object, count) {
    let sum = 0;
    for (let i = 0; i < count; i++) sum += object.wrap();
    return sum;
  }
  const selector = makeSelector(7);
  const pick = { value: 7, left: 2, right: 3, choose: selector.read, wrap: selectWrapper };
  for (let i = 0; i < 150; i++) { pick.choose(); pick.wrap(); }
  assert.strictEqual(selectorDriver(pick, 1000), 2000);
  selector.set(9);
  assert.strictEqual(selectorDriver(pick, 300), 900);
  pick.choose = makeSelector(7).read;
  assert.strictEqual(selectorDriver(pick, 300), 600, 'same code must use the actual closure');
  let coercions = 0, leftGets = 0;
  pick.value = { valueOf() { coercions++; return 7; } };
  Object.defineProperty(pick, 'left', { get() { leftGets++; return 2; } });
  assert.strictEqual(selectorDriver(pick, 100), 200);
  assert.strictEqual(coercions, 100, 'fallback must occur before loose-equality coercion');
  assert.strictEqual(leftGets, 100);
  console.log('PASS nested reader guards, arguments, bound receivers and effects');
} else {
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, [__filename, 'child'], {
    encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
    timeout: 60000, maxBuffer: 32 * 1024 * 1024,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr));
  assert.match(child.stdout, /PASS nested reader/);
  for (const name of ['readerDriver', 'selectorDriver']) {
    const modules = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`, 'g'));
    assert.ok(modules && modules.length, `${name}: driver must compile`);
    const ids = new Set([...modules.join('\n').matchAll(/\binl(\d+)_s0\b/g)].map(match => match[1]));
    assert.ok(ids.size >= 2, `${name}: single-call-site driver should contain outer and nested bodies`);
  }
  console.log('PASS nested reader code generation');
}
