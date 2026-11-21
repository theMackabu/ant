const assert = require('node:assert');

const ROUNDS = 6000;
const SWITCH = 4000;

class Base {
  constructor(x) {
    this.x = x;
    this.target = new.target;
  }
}

class Derived extends Base {
  constructor(x) {
    super(x);
    this.y = 1;
  }
}

function Plain(x) {
  this.x = x;
  this.target = new.target;
}

function sweep(make, check) {
  for (let i = 0; i < ROUNDS; i++) check(make(i), i);
}

{
  const handler = {};
  const P = new Proxy(Base, handler);
  let trapped = 0;
  sweep(
    i => {
      if (i === SWITCH) handler.construct = (target, args, newTarget) => {
        trapped++;
        return Reflect.construct(target, args, newTarget);
      };
      return new P(i);
    },
    (object, i) => {
      assert.strictEqual(object.x, i);
      assert.strictEqual(object.target, P);
      assert.strictEqual(Object.getPrototypeOf(object), Base.prototype);
    }
  );
  assert.strictEqual(trapped, ROUNDS - SWITCH);
  delete handler.construct;
  assert.strictEqual(new P(1).x, 1);
  assert.strictEqual(trapped, ROUNDS - SWITCH);
}

{
  const shared = {};
  const handler = Object.create(shared);
  const P = new Proxy(Derived, handler);
  let trapped = 0;
  sweep(
    i => {
      if (i === SWITCH) shared.construct = (target, args, newTarget) => {
        trapped++;
        return Reflect.construct(target, args, newTarget);
      };
      return new P(i);
    },
    (object, i) => {
      assert.strictEqual(object.x, i);
      assert.strictEqual(object.y, 1);
      assert.strictEqual(object.target, P);
      assert.strictEqual(Object.getPrototypeOf(object), Derived.prototype);
    }
  );
  assert.strictEqual(trapped, ROUNDS - SWITCH);
}

{
  const P = new Proxy(Plain, {});
  let trapped = 0;
  sweep(
    i => {
      if (i === SWITCH) Object.prototype.construct = function (target, args, newTarget) {
        trapped++;
        return Reflect.construct(target, args, newTarget);
      };
      return new P(i);
    },
    object => assert.strictEqual(Object.getPrototypeOf(object), Plain.prototype)
  );
  delete Object.prototype.construct;
  assert.strictEqual(trapped, ROUNDS - SWITCH);
  new P(1);
  assert.strictEqual(trapped, ROUNDS - SWITCH);
}

{
  const handler = {};
  const P = new Proxy(Plain, handler);
  const replacement = { marker: true };
  let reads = 0;
  sweep(
    i => {
      if (i === SWITCH) handler.get = (target, key, receiver) => {
        if (key === 'prototype') reads++;
        return key === 'prototype' ? replacement : Reflect.get(target, key, receiver);
      };
      return new P(i);
    },
    (object, i) => assert.strictEqual(Object.getPrototypeOf(object), i < SWITCH ? Plain.prototype : replacement)
  );
  assert.strictEqual(reads, ROUNDS - SWITCH);
}

{
  function Target() {}
  const first = Target.prototype;
  const second = { second: true };
  const P = new Proxy(Target, {});
  sweep(
    i => {
      if (i === SWITCH) Target.prototype = second;
      return new P();
    },
    (object, i) => assert.strictEqual(Object.getPrototypeOf(object), i < SWITCH ? first : second)
  );
  Target.prototype = 5;
  assert.strictEqual(Object.getPrototypeOf(new P()), Object.prototype);
}

{
  const log = [];
  const handler = new Proxy({}, {
    get(target, key) {
      log.push(key);
      return undefined;
    },
  });
  const P = new Proxy(Base, handler);
  new P(1);
  assert.deepStrictEqual(log, ['construct', 'get']);
}

{
  const log = [];
  const handler = {
    get construct() {
      log.push('construct');
      return undefined;
    },
    get get() {
      log.push('get');
      return undefined;
    },
  };
  const P = new Proxy(Plain, handler);
  for (let i = 0; i < 3; i++) new P(i);
  assert.deepStrictEqual(log, ['construct', 'get', 'construct', 'get', 'construct', 'get']);
}

{
  const P = new Proxy(Plain, { construct: null, get: null });
  assert.strictEqual(new P(7).x, 7);
  assert.throws(() => new (new Proxy(Plain, { construct: 1 }))(), TypeError);
  assert.throws(() => new (new Proxy(Plain, { get: 1 }))(), TypeError);
  assert.throws(() => new (new Proxy(() => {}, {}))(), TypeError);
}

{
  const { proxy, revoke } = Proxy.revocable(Plain, {});
  for (let i = 0; i < ROUNDS; i++) assert.strictEqual(new proxy(i).x, i);
  revoke();
  assert.throws(() => new proxy(1), TypeError);
}

{
  const pair = Proxy.revocable(Plain, {});
  function Revoker() {
    pair.revoke();
    this.done = true;
  }
  const P = new Proxy(Revoker, {});
  const made = Reflect.construct(P, [], pair.proxy);
  assert.strictEqual(made.done, true);
  assert.strictEqual(Object.getPrototypeOf(made), Plain.prototype);
  assert.throws(() => Reflect.construct(P, [], pair.proxy), TypeError);
  assert.throws(() => Reflect.construct(Plain, [], pair.proxy), TypeError);
}

{
  const inner = new Proxy(Base, {});
  const outer = new Proxy(inner, {});
  sweep(
    i => new outer(i),
    (object, i) => {
      assert.strictEqual(object.x, i);
      assert.strictEqual(object.target, outer);
      assert.strictEqual(Object.getPrototypeOf(object), Base.prototype);
    }
  );
}

{
  const proto = { custom: true };
  const bound = Plain.bind(null);
  const viaTrap = new Proxy(Plain, { get: (target, key) => (key === 'prototype' ? proto : target[key]) });
  const viaPlain = new Proxy(Derived, {});
  class Other {}
  function Fn() {}
  const cases = [
    [bound, Object.prototype],
    [viaTrap, proto],
    [viaPlain, Derived.prototype],
    [Other, Other.prototype],
    [Fn, Fn.prototype],
  ];
  const P = new Proxy(Plain, {});
  for (let i = 0; i < ROUNDS; i++) {
    const [newTarget, expected] = cases[i % cases.length];
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Plain, [i], newTarget)), expected);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(P, [i], newTarget)), expected);
    assert.strictEqual(Reflect.construct(P, [i], newTarget).target, newTarget);
  }
}

{
  class Sub extends Derived {
    constructor(x) {
      return Reflect.construct(Derived, [x], new.target);
    }
  }
  class Leaf extends Sub {}
  function Legacy(x) {
    return Reflect.construct(Plain, [x], new.target);
  }
  Legacy.prototype = Object.create(Plain.prototype);
  const swapped = { swapped: true };

  sweep(
    i => new Sub(i),
    (object, i) => {
      assert.strictEqual(object.x, i);
      assert.strictEqual(object.target, Sub);
      assert.strictEqual(Object.getPrototypeOf(object), Sub.prototype);
    }
  );
  sweep(
    i => new Leaf(i),
    object => {
      assert.strictEqual(object.target, Leaf);
      assert.strictEqual(Object.getPrototypeOf(object), Leaf.prototype);
    }
  );
  const first = Legacy.prototype;
  sweep(
    i => {
      if (i === SWITCH) Legacy.prototype = swapped;
      return new Legacy(i);
    },
    (object, i) => {
      assert.strictEqual(object.x, i);
      assert.strictEqual(object.target, Legacy);
      assert.strictEqual(Object.getPrototypeOf(object), i < SWITCH ? first : swapped);
    }
  );
  assert.throws(() => Legacy(1));
}

{
  function make(args, newTarget) {
    return Reflect.construct(Plain, args, newTarget);
  }
  function literal(a, newTarget) {
    return Reflect.construct(Plain, [a], newTarget);
  }
  function aliased(a, newTarget) {
    let args;
    return Reflect.construct(Plain, (args = [a]), (args.push(a + 1), (args[0] = a + 2), newTarget));
  }
  function grown(a) {
    const args = [a];
    args.length = 0;
    return Reflect.construct(Array, args);
  }
  function holes(a) {
    return Reflect.construct(Array, [a, , a]);
  }
  function parameter(args, a) {
    return Reflect.construct(Plain, (args = [a]), (arguments[0].unshift(a + 1), Plain));
  }
  function empty() {
    return Reflect.construct(Array, [], Plain);
  }
  function two(a, b) {
    return Reflect.construct(Array, [a, b]);
  }
  for (let i = 0; i < ROUNDS; i++) {
    assert.strictEqual(make([i], Plain).x, i);
    assert.strictEqual(literal(i, Derived).x, i);
    assert.strictEqual(Object.getPrototypeOf(literal(i, Derived)), Derived.prototype);
    assert.strictEqual(aliased(i, Plain).x, i + 2);
    assert.strictEqual(grown(i).length, 0);
    assert.strictEqual(holes(i).length, 3);
    assert.strictEqual(1 in holes(i), true);
    assert.strictEqual(parameter(null, i).x, i + 1);
    assert.strictEqual(Object.getPrototypeOf(empty()), Plain.prototype);
    assert.strictEqual(empty().length, 0);
    assert.deepStrictEqual(two(i, 2), [i, 2]);
  }
  Array.prototype[1] = 'inherited';
  for (let i = 0; i < 3; i++) assert.deepStrictEqual(holes(i), [i, 'inherited', i]);
  delete Array.prototype[1];
  assert.throws(() => literal(1, undefined));
  assert.throws(() => literal(1, () => {}));
  assert.throws(() => literal(1, {}));
  assert.throws(() => Reflect.construct(() => {}, [1]));
  assert.throws(() => Reflect.construct(Plain, 1), TypeError);
}

{
  function literal(a, newTarget) {
    return Reflect.construct(Plain, [a], newTarget);
  }
  function implicit(target, a) {
    return Reflect.construct(target, [a]);
  }
  for (let i = 0; i < ROUNDS; i++) {
    literal(i, Derived);
    implicit(Plain, i);
  }
  for (const invalid of [undefined, null, 1, {}, () => {}]) {
    assert.throws(() => literal(1, invalid), TypeError);
    assert.throws(() => implicit(invalid, 1), TypeError);
  }

  function rejected(invalid) {
    let thrown = 0;
    for (let i = 0; i < ROUNDS; i++) {
      try {
        Reflect.construct(Plain, [i], i === ROUNDS - 1 ? invalid : Derived);
      } catch (error) {
        if (error instanceof TypeError) thrown++;
      }
      try {
        Reflect.construct(i === ROUNDS - 1 ? invalid : Plain, [i]);
      } catch (error) {
        if (error instanceof TypeError) thrown++;
      }
    }
    return thrown;
  }
  for (const invalid of [undefined, null, 1, {}, () => {}]) assert.strictEqual(rejected(invalid), 2);
}

{
  function build(a) {
    return Reflect.construct(Plain, [a], Derived);
  }
  const original = Reflect.construct;
  let replaced = 0;
  for (let i = 0; i < ROUNDS; i++) {
    if (i === SWITCH) Reflect.construct = (target, args) => {
      replaced++;
      return original(target, args);
    };
    const object = build(i);
    assert.strictEqual(object.x, i);
    assert.strictEqual(Object.getPrototypeOf(object), i < SWITCH ? Derived.prototype : Plain.prototype);
  }
  Reflect.construct = original;
  assert.strictEqual(replaced, ROUNDS - SWITCH);
}

{
  let reads = 0;
  Object.defineProperty(Array.prototype, 0, {
    configurable: true,
    get() {
      reads++;
      return 'inherited';
    },
  });
  function build(a) {
    return Reflect.construct(Plain, [a]);
  }
  for (let i = 0; i < ROUNDS; i++) {
    const object = build(i);
    assert.strictEqual(object.x, i);
    assert.strictEqual(object.target, Plain);
    assert.strictEqual(Object.getPrototypeOf(object), Plain.prototype);
  }
  delete Array.prototype[0];
  assert.strictEqual(reads, 0);
}

{
  function sum(a, b) {
    return a + b + (this === undefined ? 0 : this.bias | 0);
  }
  const handler = {};
  const P = new Proxy(sum, handler);
  let trapped = 0;
  for (let i = 0; i < ROUNDS; i++) {
    if (i === SWITCH) handler.apply = (target, receiver, args) => {
      trapped++;
      return Reflect.apply(target, receiver, args) + 1;
    };
    assert.strictEqual(P(i, 1), i + 1 + (i < SWITCH ? 0 : 1));
  }
  assert.strictEqual(trapped, ROUNDS - SWITCH);

  const calls = [];
  const viaFunction = new Proxy(function target() {}, function handler(...args) {
    calls.push(this, args.length);
    return 'handled';
  });
  assert.strictEqual(viaFunction(1, 2), 'handled');
  assert.strictEqual(calls.length, 2);
  assert.strictEqual(typeof calls[0], 'function');
  assert.strictEqual(calls[1], 0);

  const detached = function () {
    return 'handled';
  };
  Object.setPrototypeOf(detached, null);
  assert.strictEqual(new Proxy(() => 'direct', detached)(), 'direct');
}

{
  const target = {};
  const handler = {};
  const P = new Proxy(target, handler);
  let trapped = 0;
  for (let i = 0; i < ROUNDS; i++) {
    if (i === SWITCH) handler.defineProperty = (inner, key, descriptor) => {
      trapped++;
      return Reflect.defineProperty(inner, key, descriptor);
    };
    Object.defineProperty(P, 'a', { value: i, writable: true, enumerable: true, configurable: true });
    assert.strictEqual(target.a, i);
    assert.strictEqual(Reflect.defineProperty(P, 'b', { value: i, configurable: true }), true);
    assert.strictEqual(target.b, i);
  }
  assert.strictEqual(trapped, 2 * (ROUNDS - SWITCH));
  delete handler.defineProperty;
  Object.freeze(target);
  assert.throws(() => Object.defineProperty(P, 'c', { value: 1 }), TypeError);
  assert.strictEqual(Reflect.defineProperty(P, 'c', { value: 1 }), false);

  const revocable = Proxy.revocable({}, {});
  Object.defineProperty(revocable.proxy, 'a', { value: 1 });
  revocable.revoke();
  assert.throws(() => Object.defineProperty(revocable.proxy, 'a', { value: 1 }), TypeError);
}

{
  const created = [];
  const inherited = { inherited: true };
  const unowned = Plain.bind(null);
  Function.prototype.prototype = inherited;
  for (let i = 0; i < 100; i++) created.push(new Proxy({ i }, {}));
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Plain, [], unowned)), inherited);
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(new Proxy(Plain, {}), [], new Proxy(unowned, {}))), inherited);
  delete Function.prototype.prototype;
  assert.strictEqual(created[42].i, 42);
  assert.strictEqual(Object.getPrototypeOf(created[42]), Object.prototype);
  const bound = Plain.bind(null);
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Object, [], bound)), Object.prototype);
}

console.log('ok');
