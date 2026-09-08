const assert = require('node:assert');

function ordinary() {
  assert.strictEqual(new.target, undefined);
  return Number();
}

// Native constructors must retain their target across JavaScript coercion,
// ordinary native calls, nested construction, and exceptions.
class WrappedNumber extends Number {}
for (let i = 0; i < 300; i++) {
  const value = new WrappedNumber({
    valueOf() {
      assert.strictEqual(new.target, undefined);
      assert.strictEqual(ordinary(), 0);
      new String('nested');
      try {
        new Map(1);
      } catch (error) {
        assert.ok(error instanceof TypeError);
      }
      return 42;
    },
  });
  assert.strictEqual(Object.getPrototypeOf(value), WrappedNumber.prototype);
  assert.strictEqual(Number.prototype.valueOf.call(value), 42);
}

// A builtin used as a coercion method is an ordinary call even when its
// caller is a native constructor.
const zero = new WrappedNumber({ valueOf: Number });
assert.strictEqual(Number.prototype.valueOf.call(zero), 0);
assert.strictEqual(Object.getPrototypeOf(zero), WrappedNumber.prototype);

function Capture(value) {
  const before = new.target;
  const kind = typeof value;
  ordinary();
  new Number(7);
  this.before = before;
  this.after = new.target;
  this.read = () => new.target;
  this.kind = kind;
}

function check(value, target) {
  assert.strictEqual(value.before, target);
  assert.strictEqual(value.after, target);
  assert.strictEqual(value.read(), target);
}

// Run past the JIT threshold, then change the argument type.
for (let i = 0; i < 500; i++) check(new Capture(i), Capture);
const changed = new Capture('changed');
check(changed, Capture);
assert.strictEqual(changed.kind, 'string');
check(new Capture(...[1]), Capture);

const Bound = Capture.bind(null, 1);
check(new Bound(), Capture);
const BoundAgain = Bound.bind(null);
check(new BoundAgain(), Capture);

function Alternate() {}
check(Reflect.construct(Capture, [1], Alternate), Alternate);
check(Reflect.construct(Bound, [], Alternate), Alternate);

const Forwarded = new Proxy(Capture, {});
check(new Forwarded(1), Forwarded);
const Trapped = new Proxy(Capture, {
  construct(target, args, newTarget) {
    assert.strictEqual(new.target, undefined);
    ordinary();
    return Reflect.construct(target, args, newTarget);
  },
});
check(new Trapped(1), Trapped);

class Implicit extends Capture {}
class Explicit extends Capture {
  constructor(...args) {
    ordinary();
    super(...args);
    assert.strictEqual(new.target, Explicit);
  }
}
check(new Implicit(1), Implicit);
check(new Explicit(1), Explicit);

function Abrupt() {
  new WrappedNumber(1);
  throw new Error('constructor failed');
}
assert.throws(() => new Abrupt(), /constructor failed/);
assert.strictEqual(ordinary(), 0);
assert.throws(() => Map(), TypeError);

// Direct eval inherits the lexical target even when the caller contains no
// literal new.target. Cover both inlined literals and runtime source strings.
function EvalLiteral() {
  return eval('new.target');
}
function EvalSource(source, nested) {
  return eval(source);
}
function EvalArrow(source) {
  return () => eval(source);
}
assert.strictEqual(new EvalLiteral(), EvalLiteral);
assert.strictEqual(new EvalSource('new.target'), EvalSource);
assert.strictEqual(new EvalSource('0; new.target'), EvalSource);
assert.strictEqual(new EvalSource('"use strict"; new.target'), EvalSource);
assert.strictEqual(new EvalSource('eval(nested)', 'new.target'), EvalSource);
assert.strictEqual(new EvalArrow('new.target')(), EvalArrow);
assert.strictEqual(new EvalSource('() => new.target')(), EvalSource);
assert.strictEqual(new EvalSource('() => eval("new.target")')(), EvalSource);
assert.strictEqual(Reflect.construct(EvalSource, ['new.target'], Alternate), Alternate);
assert.strictEqual(EvalSource('new.target'), undefined);
assert.strictEqual(EvalSource('() => new.target')(), undefined);
assert.strictEqual(EvalSource(42), 42);

// An ordinary function creates its own target boundary, including when it was
// created by eval inside a constructor.
function EvalBoundary() {
  assert.strictEqual(EvalSource('new.target'), undefined);
  return function (source) { return eval(source); };
}
assert.strictEqual(new EvalBoundary()('new.target'), undefined);
assert.strictEqual(
  new EvalSource('(function () { return eval("new.target"); })')(),
  undefined,
);

// The hidden target must exist before defaults or hoisted closures use it.
function EvalDefault(source, target = eval(source)) {
  this.target = target;
}
function TargetDefault(target = new.target) {
  this.target = target;
}
function EvalDefaultArrow(read = () => eval('new.target')) {
  return read;
}
function EvalHoisted() {
  function read() { return new.target; }
  return [read(), eval('new.target')];
}
assert.strictEqual(new EvalDefault('new.target').target, EvalDefault);
assert.strictEqual(new TargetDefault().target, TargetDefault);
assert.strictEqual(new EvalDefaultArrow()(), EvalDefaultArrow);
assert.deepStrictEqual(new EvalHoisted(), [undefined, EvalHoisted]);

class EvalDerived extends Capture {
  constructor(source) {
    const target = eval(source);
    super(1);
    assert.strictEqual(target, EvalDerived);
  }
}
check(new EvalDerived('new.target'), EvalDerived);

// Trigger OSR and an arithmetic bailout before super(). Both the constructor
// target and superclass must reach the interpreter continuation.
const coercible = {
  valueOf() {
    new Number(1);
    return 7;
  },
};
class Base {
  constructor(total) {
    this.target = new.target;
    this.total = total;
  }
}
class Resume extends Base {
  constructor() {
    let total = 0;
    for (let i = 0; i < 700; i++) total += i === 600 ? coercible : 1;
    super(total);
    this.read = () => new.target;
  }
}
const value = new Resume();
assert.strictEqual(value.target, Resume);
assert.strictEqual(value.read(), Resume);
assert.strictEqual(value.total, 706);

function Suspended() {
  this.arrow = async () => {
    await 0;
    return new.target;
  };
  this.ordinary = async function () {
    await 0;
    return new.target;
  };
  this.generator = function* () {
    yield new.target;
    return new.target;
  };
}

const suspended = new Suspended();
const iterator = suspended.generator();
assert.strictEqual(iterator.next().value, undefined);
assert.strictEqual(iterator.next().value, undefined);
Promise.all([suspended.arrow(), suspended.ordinary()]).then(values => {
  assert.strictEqual(values[0], Suspended);
  assert.strictEqual(values[1], undefined);
  console.log('new-target:frames:ok');
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
