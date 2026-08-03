// A named class *expression* binds its name inside the class body only. It must
// never write that name into the enclosing scope, which used to clobber unrelated
// bindings of the same name (a hoisted function, a let, a const, ...).
const assert = require('node:assert');

function shape(a, b) {
  return 'fn:' + a + b;
}

var Err = class shape {
  status = 'error';
  constructor(error) {
    this.error = error;
  }
  wrap(value) {
    return new shape(value);
  }
  *[Symbol.iterator]() {
    return this.error;
  }
};

assert.strictEqual(typeof shape, 'function');
assert.notStrictEqual(shape, Err, 'class expression name leaked into the outer scope');
assert.strictEqual(shape(1, 2), 'fn:12');
assert.strictEqual(new Err('boom').error, 'boom');
assert.strictEqual(new Err('boom').status, 'error');
assert.ok(new Err('boom').wrap('inner') instanceof Err, 'inner class binding is broken');

let counter = 1;
const Counter = class counter {};
assert.strictEqual(counter, 1, 'class expression name overwrote a let binding');
assert.strictEqual(typeof Counter, 'function');

const holder = { kind: 'plain' };
const Holder = class holder {};
assert.deepStrictEqual(holder, { kind: 'plain' }, 'class expression name overwrote a const binding');
assert.strictEqual(typeof Holder, 'function');

// declarations still bind in the enclosing scope
class Declared {
  value() {
    return 7;
  }
}
assert.strictEqual(typeof Declared, 'function');
assert.strictEqual(new Declared().value(), 7);

// a declaration shadowed by a later class expression of the same name
class Same {}
const SameExpr = class Same {};
assert.notStrictEqual(Same, SameExpr, 'class expression name overwrote a class declaration');

// the inner name is immutable and scoped to the body
const Immutable = class Self {
  static self() {
    return Self;
  }
};
assert.strictEqual(Immutable.self(), Immutable);
assert.strictEqual(typeof globalThis.Self, 'undefined');

console.log('class expression name scope ok');
