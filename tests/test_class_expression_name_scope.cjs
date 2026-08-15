const assert = require('node:assert');

// A named class expression owns an inner self-binding; it must not bind or
// overwrite the same name in the enclosing scope.
function shape(a, b) {
  return 'fn:' + a + b;
}

const Err = class shape {
  status = 'error';

  constructor(error) {
    this.error = error;
  }

  wrap(value) {
    return new shape(value);
  }
};

assert.strictEqual(typeof shape, 'function');
assert.notStrictEqual(shape, Err);
assert.strictEqual(shape(1, 2), 'fn:12');
assert.strictEqual(new Err('boom').error, 'boom');
assert.strictEqual(new Err('boom').status, 'error');
assert.ok(new Err('boom').wrap('inner') instanceof Err);

let counter = 1;
const Counter = class counter {};
assert.strictEqual(counter, 1);
assert.strictEqual(typeof Counter, 'function');

const holder = { kind: 'plain' };
const Holder = class holder {};
assert.deepStrictEqual(holder, { kind: 'plain' });
assert.strictEqual(typeof Holder, 'function');

// Class declarations still create their enclosing lexical binding.
class Declared {
  value() {
    return 7;
  }
}
assert.strictEqual(typeof Declared, 'function');
assert.strictEqual(new Declared().value(), 7);

class Same {}
const SameExpr = class Same {};
assert.notStrictEqual(Same, SameExpr);

const Immutable = class Self {
  static self() {
    return Self;
  }
};
assert.strictEqual(Immutable.self(), Immutable);
assert.strictEqual(typeof globalThis.Self, 'undefined');

console.log('class expression name scope ok');
