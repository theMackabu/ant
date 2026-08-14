const assert = require('node:assert');

const expectedMessage = "Class constructor cannot be invoked without 'new'";
const expectedNamedMessageSuffix = " cannot be invoked without 'new'";

let validationRuns = 0;

function assertClassCallThrows(call, label) {
  assert.throws(call, error => {
    validationRuns++;
    assert.strictEqual(error.name, 'TypeError', label);
    assert.ok(
      error.message === expectedMessage || (error.message.startsWith('Class constructor ') && error.message.endsWith(expectedNamedMessageSuffix)),
      label
    );
    return true;
  });
}

let parameterRuns = 0;
let bodyRuns = 0;
class Explicit {
  constructor(value = (parameterRuns++, 1)) {
    bodyRuns++;
    this.value = value;
  }
}

assertClassCallThrows(() => Explicit(), 'direct call');
assertClassCallThrows(() => Explicit.call(null), 'Function.prototype.call');
assertClassCallThrows(() => Reflect.apply(Explicit, null, []), 'Reflect.apply');
assertClassCallThrows(() => ({ Explicit }).Explicit(), 'method-position call');
assertClassCallThrows(() => Explicit.bind(null)(), 'bound ordinary call');
assert.strictEqual(parameterRuns, 0, 'parameter defaults ran before the guard');
assert.strictEqual(bodyRuns, 0, 'constructor body ran before the guard');

const explicit = new Explicit();
assert.strictEqual(explicit.value, 1);
assert.strictEqual(parameterRuns, 1);
assert.strictEqual(bodyRuns, 1);
assert.ok(Reflect.construct(Explicit, [2]) instanceof Explicit);
assert.ok(new (Explicit.bind(null, 3))() instanceof Explicit);

let fieldRuns = 0;
class WithField {
  value = (fieldRuns++, 1);
}
assertClassCallThrows(() => WithField(), 'synthesized field constructor');
assert.strictEqual(fieldRuns, 0, 'field initializer ran before the guard');
assert.strictEqual(new WithField().value, 1);

class Base {}
class Derived extends Base {
  constructor() {
    super();
  }
}
assertClassCallThrows(() => Derived(), 'derived explicit constructor');
assert.ok(new Derived() instanceof Base);

class DerivedWithField extends Base {
  value = 1;
}
assertClassCallThrows(() => DerivedWithField(), 'derived synthesized constructor');
assert.strictEqual(new DerivedWithField().value, 1);

class ArrowBase {
  constructor(value) {
    this.value = value;
    this.seenNewTarget = new.target;
  }
}
class ArrowDerived extends ArrowBase {
  constructor(value) {
    const callSuper = () => super(value);
    assert.strictEqual(callSuper(), this, 'arrow super() result');
  }
}
for (let i = 0; i < 5000; i++) {
  const value = new ArrowDerived(i);
  assert.strictEqual(value.value, i, 'arrow super() argument');
  assert.strictEqual(value.seenNewTarget, ArrowDerived, 'arrow super() new.target');
}

class SpreadArrowDerived extends ArrowBase {
  constructor(...args) {
    const callSuper = () => super(...args);
    callSuper();
  }
}
const spreadArrow = new SpreadArrowDerived(42);
assert.strictEqual(spreadArrow.value, 42, 'spread arrow super() argument');
assert.strictEqual(
  spreadArrow.seenNewTarget, SpreadArrowDerived,
  'spread arrow super() new.target'
);

class NestedArrowDerived extends ArrowBase {
  constructor(value) {
    const makeCallSuper = () => () => super(value);
    makeCallSuper()();
  }
}
const nestedArrow = new NestedArrowDerived(43);
assert.strictEqual(nestedArrow.value, 43, 'nested arrow super() argument');
assert.strictEqual(
  nestedArrow.seenNewTarget, NestedArrowDerived,
  'nested arrow super() new.target'
);

// Compile the constructor before exercising the invalid ordinary-call path.
class Hot {
  constructor(value) {
    this.value = value;
  }
}
for (let i = 0; i < 5000; i++) assert.strictEqual(new Hot(i).value, i);
assertClassCallThrows(() => Hot(1), 'JIT-compiled constructor');
assert.strictEqual(validationRuns, 9, 'every throw validator ran');

console.log('class constructor call guard ok');
