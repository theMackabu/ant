const assert = require('node:assert');

const writers = {
  assign(F, p) { F.prototype = p; },
  define(F, p) { Object.defineProperty(F, 'prototype', { value: p }); },
  reflectSet(F, p) { Reflect.set(F, 'prototype', p); },
  reflectDefine(F, p) { Reflect.defineProperty(F, 'prototype', { value: p }); },
  objectAssign(F, p) { Object.assign(F, { prototype: p }); },
  computed(F, p) { const k = 'proto' + 'type'; F[k] = p; },
  nested(F, p) { Reflect.construct(class extends Object { constructor() { F.prototype = p; super(); } }, [], function () {}); },
  nestedPlain(F, p) { new (class extends Object { constructor() { F.prototype = p; super(); } })(); },
  none() {},
};

class Base { constructor(v) { this.v = v; this.seen = Object.getPrototypeOf(this); } }

for (const [name, write] of Object.entries(writers)) {
  function F() {}
  const first = F.prototype;
  let next = null;
  let when = 'none';

  class Direct extends Base {
    constructor(v) { if (when === 'before') write(F, next); super(v); if (when === 'after') write(F, next); this.d = 1; }
  }
  class ArgTime extends Base {
    constructor(v) { super((when === 'before' ? write(F, next) : 0, v)); if (when === 'after') write(F, next); this.d = 1; }
  }
  class Middle extends Base { constructor(v) { if (when === 'middle') write(F, next); super(v); this.m = 1; } }
  class Top extends Middle { constructor(v) { if (when === 'before') write(F, next); super(v); if (when === 'after') write(F, next); this.d = 1; } }
  class Forwarding extends Top {}
  class Arrow extends Base { constructor(v) { const run = () => super(v); if (when === 'before') write(F, next); run(); if (when === 'after') write(F, next); this.d = 1; } }
  class Twice extends Base {
    constructor(v) { super(v); this.d = 1; if (when === 'before') { write(F, next); try { super(v); } catch (error) { this.again = error.constructor.name; } } }
  }

  for (const C of [Direct, ArgTime, Top, Forwarding, Arrow]) {
    for (let round = 0; round < 400; round++) {
      const mode = round % 7 === 3 ? 'before' : round % 7 === 5 ? 'after' : C === Top && round % 7 === 6 ? 'middle' : 'none';
      const before = F.prototype;
      next = { round };
      when = name === 'none' ? 'none' : mode;
      const made = Reflect.construct(C, [round], F);
      const wrote = when !== 'none';
      const expected = wrote && when !== 'after' ? next : before;
      assert.strictEqual(Object.getPrototypeOf(made), expected, `${name} ${C.name} ${when} ${round}`);
      assert.strictEqual(made.seen, expected, `${name} ${C.name} ${when} ${round} seen`);
      assert.strictEqual(made.v, round);
      assert.strictEqual(made.d, 1);
      assert.strictEqual(F.prototype, wrote ? next : before, `${name} ${C.name} ${when} ${round} written`);
      when = 'none';
      assert.strictEqual(Object.getPrototypeOf(new C(1)), C.prototype);
    }
  }

  for (let round = 0; round < 60; round++) {
    const before = F.prototype;
    next = { round };
    when = name === 'none' ? 'none' : round % 3 === 1 ? 'before' : 'none';
    const made = Reflect.construct(Twice, [round], F);
    assert.strictEqual(Object.getPrototypeOf(made), before, `${name} twice ${round}`);
    if (when === 'before') assert.strictEqual(made.again, 'ReferenceError');
    when = 'none';
  }
  assert.notStrictEqual(first, null);
}

{
  function Outer() {}
  function Inner() {}
  const swapped = { swapped: true };
  class A { constructor() { this.proto = Object.getPrototypeOf(this); } }
  class In extends A { constructor(change) { if (change) Outer.prototype = swapped; super(); } }
  class Out extends A { constructor(change) { const inner = Reflect.construct(In, [change], Inner); super(); this.inner = inner; } }
  for (let round = 0; round < 300; round++) {
    const change = round % 5 === 2;
    const before = Outer.prototype;
    const made = Reflect.construct(Out, [change], Outer);
    assert.strictEqual(made.proto, change ? swapped : before);
    assert.strictEqual(made.inner.proto, Inner.prototype);
    Outer.prototype = { round };
  }
}

{
  function F() {}
  const order = [];
  const watched = new Proxy(F, { get(target, key, receiver) { if (key === 'prototype') order.push('get'); return Reflect.get(target, key, receiver); } });
  class A { constructor() { order.push('base'); } }
  class D extends A { constructor() { order.push('body'); super(); order.push('after'); } }
  for (let round = 0; round < 200; round++) {
    order.length = 0;
    const made = Reflect.construct(D, [], round & 1 ? watched : F);
    assert.strictEqual(Object.getPrototypeOf(made), F.prototype);
    assert.deepStrictEqual(order, round & 1 ? ['body', 'get', 'base', 'after'] : ['body', 'base', 'after']);
  }
}

{
  function F() {}
  class A {}
  class Throws extends A { constructor() { F.prototype = { late: true }; throw new RangeError('stop'); } }
  class Next extends A { constructor() { super(); } }
  for (let round = 0; round < 200; round++) {
    assert.throws(() => Reflect.construct(Throws, [], F), RangeError);
    const expected = F.prototype;
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Next, [], F)), expected);
    assert.strictEqual(Object.getPrototypeOf(new Next()), Next.prototype);
  }
}

console.log('ok');
