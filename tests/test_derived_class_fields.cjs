const assert = require('node:assert');

class Base {
  broken = false;
}
class Derived extends Base {
  broken = true;
  constructor() {
    super();
    assert.strictEqual(this.broken, true);
  }
}
assert.strictEqual(new Derived().broken, true);

const events = [];
class OrderedBase {
  value = (events.push('base field'), 1);
  constructor(value) {
    events.push('base body');
    this.value = value;
  }
}
class OrderedDerived extends OrderedBase {
  value = (events.push('derived field'), this.value + 1);
  constructor(spread) {
    events.push('before super');
    const result = spread ? super(...[4]) : super(4);
    assert.strictEqual(result, this);
    assert.strictEqual(this.value, 5);
    events.push('after super');
  }
}
for (const spread of [false, true]) {
  events.length = 0;
  assert.strictEqual(new OrderedDerived(spread).value, 5);
  assert.deepStrictEqual(events, [
    'before super', 'base field', 'base body', 'derived field', 'after super'
  ]);
}

class Returning extends Base {
  broken = true;
  constructor() { return super(); }
}
assert.strictEqual(new Returning().broken, true);

class ReplacementBase {
  constructor() { return { value: 7 }; }
}
class ReplacementDerived extends ReplacementBase {
  value = this.value + 1;
  #secret = 9;
  constructor() {
    super();
    assert.strictEqual(this.value, 8);
    assert.strictEqual(this.#secret, 9);
  }
}
assert.strictEqual(new ReplacementDerived().value, 8);

let initialized = 0;
class ThrowingBase {
  constructor() { throw new Error('base failed'); }
}
class ThrowingDerived extends ThrowingBase {
  field = ++initialized;
  constructor() { super(); }
}
assert.throws(() => new ThrowingDerived(), /base failed/);
assert.strictEqual(initialized, 0);
class Skipped extends Base {
  field = ++initialized;
  constructor() { return {}; }
}
assert.deepStrictEqual(new Skipped(), {});
assert.strictEqual(initialized, 0);

let keyCalls = 0;
class Computed extends Base {
  [(keyCalls++, 'broken')] = true;
  constructor() { super(); }
}
assert.strictEqual(new Computed().broken, true);
assert.strictEqual(new Computed().broken, true);
assert.strictEqual(keyCalls, 1);

class ArrowSuper extends Base {
  ['broken'] = true;
  constructor() {
    const initialize = () => super();
    initialize();
    assert.strictEqual(this.broken, true);
  }
}
assert.strictEqual(new ArrowSuper().broken, true);

class Implicit extends Derived {
  broken = 'implicit';
}
assert.strictEqual(new Implicit().broken, 'implicit');
class Third extends Derived {
  broken = 'third';
  constructor() {
    super();
    assert.strictEqual(this.broken, 'third');
  }
}
assert.strictEqual(new Third().broken, 'third');

console.log('derived class fields: ok');
