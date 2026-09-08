const assert = require('node:assert');

function Plain(value) {
  this.value = value + 1;
}
function Target(value) {
  this.value = value + 1;
  this.target = new.target;
  this.read = () => new.target;
}
class Base {
  constructor(value) { this.value = value; }
  read() { return this.value; }
}
class Derived extends Base {
  constructor(value) {
    const changed = value + 1;
    super(changed);
    this.target = new.target;
  }
  read(value) {
    const changed = value + 1;
    return [changed, super.read()];
  }
}
for (let i = 0; i < 300; i++) {
  assert.strictEqual(new Plain(i).value, i + 1);
  const target = new Target(i);
  assert.strictEqual(target.target, Target);
  assert.strictEqual(target.read(), Target);
  assert.strictEqual(new Derived(i).read(i)[0], i + 1);
}

// Change the arithmetic types after JIT warmup, then use constructor context.
assert.strictEqual(new Plain('x').value, 'x1');
function Alternate() {}
const target = Reflect.construct(Target, ['x'], Alternate);
assert.strictEqual(target.value, 'x1');
assert.strictEqual(target.target, Alternate);
assert.strictEqual(target.read(), Alternate);
const derived = new Derived('x');
assert.strictEqual(derived.target, Derived);
assert.deepStrictEqual(derived.read('y'), ['y1', 'x1']);
assert.throws(() => Derived(1), TypeError);
console.log('jit:bailout-call-context:ok');
