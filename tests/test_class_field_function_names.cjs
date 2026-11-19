const assert = require('node:assert');

const WARM = 3000;

{
  const sym = Symbol('key');
  function declared() {}
  const existing = () => 1;

  class Names {
    arrow = () => 1;
    fn = function () {};
    klass = class {};
    ['computed'] = () => 1;
    7 = () => 1;
    [sym] = () => 1;
    'two words' = () => 1;
    declared = declared;
    existing = existing;
    inner = function own() {};
    ownName = class { static name = 'own' };
    #hidden = () => 1;
    static fixed = () => 3;
    static [sym] = class {};
    static 8 = function () {};
    static #secret = () => 1;
    static secret() { return Names.#secret.name; }
    hidden() { return this.#hidden.name; }
  }
  function makeNames() { return new Names(); }
  const first = makeNames();
  assert.deepStrictEqual(
    [first.computed.name, first[7].name, first[sym].name, first.declared.name, first.existing.name, first.inner.name, first.ownName.name],
    ['computed', '7', '[key]', 'declared', 'existing', 'own', 'own']
  );
  for (let i = 0; i < WARM; i++) {
    const made = makeNames();
    assert.deepStrictEqual(
      [made.arrow.name, made.fn.name, made.klass.name, made['two words'].name, made.declared.name, made.inner.name, made.hidden()],
      ['arrow', 'fn', 'klass', 'two words', 'declared', 'own', '#hidden']
    );
    assert.strictEqual(typeof made.computed, 'function');
    assert.strictEqual(typeof made[sym], 'function');
  }
  assert.deepStrictEqual([Names.fixed.name, Names[sym].name, Names[8].name, Names.secret()], ['fixed', '[key]', '8', '#secret']);
  assert.strictEqual(declared.name, 'declared');
  assert.strictEqual(existing.name, 'existing');
}

console.log('ok');
