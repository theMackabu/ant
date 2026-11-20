const assert = require('node:assert');

class Ret { constructor(o) { return o; } }
class Tagged extends Ret { tag = 1; [Symbol.for('s')] = 2; }

class Bytes extends Uint8Array { tag = 1; 0 = 7; 1 = 300; label = 'x'; [Symbol.for('s')] = 3; Infinity2 = 1; }
const bytes = new Bytes(2);
assert.deepStrictEqual([bytes[0], bytes[1], bytes.tag, bytes.label, bytes[Symbol.for('s')], bytes.Infinity2], [7, 44, 1, 'x', 3, 1]);
assert.deepStrictEqual(Object.keys(bytes), ['0', '1', 'tag', 'label', 'Infinity2']);
assert.deepStrictEqual(Object.getOwnPropertyDescriptor(bytes, 'tag'), { value: 1, writable: true, enumerable: true, configurable: true });

for (const key of ['-0', '1.5', '5', 'NaN', 'Infinity', '-Infinity']) {
  const C = (0, eval)(`(class extends Uint8Array { '${key}' = 1 })`);
  assert.throws(() => new C(2), TypeError, key);
}

for (const key of ['1e3', '01', '+1', 'x1', '1x', ' 1', '', 'Infinity!', 'NaNa', 'a'.repeat(40), '9'.repeat(40)]) {
  const C = (0, eval)(`(class extends Float64Array { '${key}' = 9 })`);
  const c = new C(2);
  assert.strictEqual(c[key], 9, key);
  assert.ok(Object.keys(c).includes(key), key);
  assert.deepStrictEqual([c[0], c[1]], [0, 0]);
}

assert.throws(() => new Tagged(Object.freeze(new Uint8Array(0))), TypeError);
assert.throws(() => new Tagged(Object.preventExtensions(new Float64Array(1))), TypeError);
assert.throws(() => new Tagged(Object.seal(new Int16Array(0))), TypeError);

const buffer = Buffer.alloc(2);
new Tagged(buffer);
assert.deepStrictEqual([buffer.tag, buffer[Symbol.for('s')], buffer[0]], [1, 2, 0]);

let hit = 0;
const withSetter = new Uint8Array(1);
Object.defineProperty(withSetter, 'tag', { set() { hit++; }, configurable: true });
new Tagged(withSetter);
assert.deepStrictEqual([hit, withSetter.tag], [0, 1]);

const pinned = new Uint8Array(1);
Object.defineProperty(pinned, 'tag', { value: 0, writable: true, configurable: false });
assert.throws(() => new Tagged(pinned), TypeError);

const args = (function () { return arguments; })(1, 2);
new Tagged(args);
assert.deepStrictEqual([args.tag, args.length, args[0]], [1, 2, 1]);

const str = new String('ab');
new Tagged(str);
assert.strictEqual(str.tag, 1);
class Len extends Ret { length = 1 }
assert.throws(() => new Len(new String('ab')), TypeError);

const vm = require('node:vm');
const sandbox = {};
new Tagged(vm.runInNewContext('globalThis', sandbox));
assert.deepStrictEqual([sandbox.tag, sandbox[Symbol.for('s')], vm.runInContext('tag', sandbox)], [1, 2, 1]);

let sum = 0;
for (let i = 0; i < 20000; i++) sum += new Bytes(2).tag;
assert.strictEqual(sum, 20000);

console.log('ok');
