const assert = require('node:assert');

function watch(target, log, onGet) {
  return new Proxy(target, {
    get(inner, key, receiver) {
      if (key === 'prototype') {
        log.push('get');
        if (onGet) onGet();
      }
      return Reflect.get(inner, key, receiver);
    },
  });
}

{
  const log = [];
  function F() {}
  class A { constructor(v) { log.push('A'); this.v = v; this.seen = Object.getPrototypeOf(this); } }
  class Implicit {}
  class Mid extends A { constructor(v) { log.push('mid'); super(v); log.push('mid-after'); } }
  class Fwd extends Mid {}
  class Top extends Fwd { constructor(v) { log.push('top'); super(v); log.push('top-after'); this.t = 1; } }
  class Leaf extends Top {}
  class OverImplicit extends Implicit { constructor() { log.push('body'); super(); log.push('after'); this.t = 1; } }
  class OverFwd extends (class extends A {}) { constructor(v) { log.push('body'); super(v); log.push('after'); this.t = 1; } }
  class Arrow extends A { constructor(v) { const run = () => super(v); log.push('body'); run(); log.push('after'); this.t = 1; } }
  const P = watch(F, log);

  const cases = [
    [Top, ['top', 'mid', 'get', 'A', 'mid-after', 'top-after']],
    [Leaf, ['top', 'mid', 'get', 'A', 'mid-after', 'top-after']],
    [Mid, ['mid', 'get', 'A', 'mid-after']],
    [Fwd, ['mid', 'get', 'A', 'mid-after']],
    [OverImplicit, ['body', 'get', 'after']],
    [OverFwd, ['body', 'get', 'A', 'after']],
    [Arrow, ['body', 'get', 'A', 'after']],
  ];
  for (let round = 0; round < 500; round++) {
    for (const [C, expected] of cases) {
      log.length = 0;
      const made = Reflect.construct(C, [round], P);
      assert.deepStrictEqual(log, expected, `${C.name} ${round}`);
      assert.strictEqual(Object.getPrototypeOf(made), F.prototype);
      if (C !== OverImplicit) assert.strictEqual(made.seen, F.prototype);
      log.length = 0;
      const plain = new C(round);
      assert.strictEqual(Object.getPrototypeOf(plain), C.prototype);
      assert.ok(!log.includes('get'));
      log.length = 0;
      const viaProxy = new (watch(C, log))(round);
      assert.strictEqual(Object.getPrototypeOf(viaProxy), C.prototype);
      assert.strictEqual(log.filter(entry => entry === 'get').length, 1);
    }
  }
}

{
  const log = [];
  function F() {}
  class A { constructor() { this.which = 'A'; } }
  class Other { constructor() { this.which = 'Other'; } }
  class D extends A { constructor() { super(); this.d = 1; } }
  let swap = false;
  const P = watch(F, log, () => { if (swap) Object.setPrototypeOf(D, Other); });
  for (let round = 0; round < 300; round++) {
    Object.setPrototypeOf(D, A);
    swap = round % 4 === 3;
    const made = Reflect.construct(D, [], P);
    assert.strictEqual(made.which, 'A', `round ${round}`);
    assert.strictEqual(made.d, 1);
    assert.strictEqual(Object.getPrototypeOf(made), F.prototype);
    if (swap) assert.strictEqual(new D().which, 'Other');
  }
}

{
  function F() {}
  class A { constructor() { this.a = 1; } }
  class D extends A { constructor() { super(); this.d = 1; } }
  let mode = 0;
  const replaced = { replaced: true };
  const P = new Proxy(F, {
    get(inner, key, receiver) {
      if (key !== 'prototype') return Reflect.get(inner, key, receiver);
      if (mode === 1) throw new RangeError('trap');
      if (mode === 2) return 5;
      if (mode === 3) return replaced;
      if (mode === 4) return Reflect.construct(D, [], F) && inner.prototype;
      return inner.prototype;
    },
  });
  for (let round = 0; round < 400; round++) {
    mode = round % 5;
    if (mode === 1) {
      assert.throws(() => Reflect.construct(D, [], P), RangeError);
      continue;
    }
    const made = Reflect.construct(D, [], P);
    const expected = mode === 2 ? Object.prototype : mode === 3 ? replaced : F.prototype;
    assert.strictEqual(Object.getPrototypeOf(made), expected, `mode ${mode} round ${round}`);
    assert.deepStrictEqual([made.a, made.d], [1, 1]);
    assert.strictEqual(Object.getPrototypeOf(new D()), D.prototype);
  }
}

{
  const log = [];
  function F() {}
  class M extends Map { constructor() { log.push('body'); super(); this.tag = 1; } }
  class E extends Error { constructor(message) { super(message); this.tag = 1; } }
  class Arr extends Array { constructor() { super(3); } }
  const P = watch(F, log);
  for (let round = 0; round < 200; round++) {
    log.length = 0;
    const map = Reflect.construct(M, [], P);
    assert.deepStrictEqual(log, ['body', 'get']);
    assert.strictEqual(Object.getPrototypeOf(map), F.prototype);
    assert.strictEqual(Map.prototype.set.call(map, 1, 2), map);
    assert.strictEqual(map.tag, 1);
    const error = Reflect.construct(E, ['m'], P);
    assert.strictEqual(error.message, 'm');
    assert.strictEqual(Object.getPrototypeOf(error), F.prototype);
    const list = Reflect.construct(Arr, [], P);
    assert.ok(Array.isArray(list));
    assert.strictEqual(list.length, 3);
    assert.strictEqual(Object.getPrototypeOf(list), F.prototype);
  }
}

{
  function F() {}
  class A {}
  class Returns extends A { constructor(kind) { if (kind === 1) return { own: true }; if (kind === 2) { super(); return 5; } if (kind === 3) return; super(); } }
  const P = new Proxy(F, {});
  const bound = F.bind(null);
  Object.defineProperty(bound, 'prototype', { get() { return F.prototype; } });
  for (const target of [P, bound]) {
    for (let round = 0; round < 200; round++) {
      assert.strictEqual(Reflect.construct(Returns, [1], target).own, true);
      assert.throws(() => Reflect.construct(Returns, [2], target), TypeError);
      assert.throws(() => Reflect.construct(Returns, [3], target), ReferenceError);
      assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Returns, [0], target)), F.prototype);
    }
  }
}

{
  function F() {}
  const swapped = { swapped: true };
  class A { constructor() { this.seen = Object.getPrototypeOf(this); } }
  class D extends A { constructor(change) { if (change) F.prototype = swapped; super(); this.d = 1; } }
  class Fwd extends D {}
  const proxied = new Proxy(D, {});
  const bound = D.bind(null);
  const boundFwd = Fwd.bind(null, false);
  for (let round = 0; round < 300; round++) {
    const change = round % 4 === 1;
    for (const target of [proxied, bound, Fwd]) {
      const before = F.prototype;
      const made = Reflect.construct(target, [change], F);
      assert.strictEqual(made.seen, change ? swapped : before, `${round}`);
      assert.strictEqual(made.d, 1);
      F.prototype = { round };
    }
    assert.strictEqual(Reflect.construct(boundFwd, [], F).seen, F.prototype);
    assert.strictEqual(Object.getPrototypeOf(new proxied(false)), D.prototype);
    assert.strictEqual(Object.getPrototypeOf(new bound(false)), D.prototype);
  }
}

{
  class A { constructor() { this.seen = Object.getPrototypeOf(this); } }
  class D extends A { constructor() { super(); this.d = 1; } }
  class E extends D { constructor() { return Reflect.construct(D, [], new.target); } }
  class G extends E {}
  function Plain() { return Reflect.construct(D, [], new.target); }
  for (let round = 0; round < 300; round++) {
    assert.strictEqual(new E().seen, E.prototype);
    assert.strictEqual(new G().seen, G.prototype);
    assert.strictEqual(new Plain().seen, Plain.prototype);
    assert.strictEqual(Reflect.construct(Array, [], D) instanceof D, true);
    assert.strictEqual(Array.isArray(Reflect.construct(Array, [], Object)), true);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Map, [], Object)), Object.prototype);
    function NoProto() {}
    NoProto.prototype = 3;
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Map, [], NoProto)), Map.prototype);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(D, [], NoProto)), Object.prototype);
  }
}

console.log('ok');
