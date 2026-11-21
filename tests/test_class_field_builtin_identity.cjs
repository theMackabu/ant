const assert = require('node:assert');

const WARM = 3000;

{
  const sym = Symbol('key');
  class Identity {
    ['computed'] = Math.max;
    0 = Math.max;
    atom = Math.max;
    [sym] = Math.max;
    #hidden = Math.max;
    static fixed = Math.min;
    static [sym] = Math.min;
    static 1 = Math.min;
    hidden() { return this.#hidden; }
  }
  function makeIdentity() { return new Identity(); }
  for (let i = 0; i < WARM; i++) {
    const made = makeIdentity();
    assert.strictEqual(made.computed, Math.max);
    assert.strictEqual(made[0], Math.max);
    assert.strictEqual(made.atom, Math.max);
    assert.strictEqual(made[sym], Math.max);
    assert.strictEqual(made.hidden(), Math.max);
  }
  assert.strictEqual(Identity.fixed, Math.min);
  assert.strictEqual(Identity[sym], Math.min);
  assert.strictEqual(Identity[1], Math.min);
  assert.strictEqual(Math.max.name, 'max');
  assert.strictEqual(Math.min.name, 'min');
}

console.log('ok');
