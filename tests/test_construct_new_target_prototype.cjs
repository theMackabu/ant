const assert = require('node:assert');

const watched = (log, target = function () {}) => new Proxy(target, {
  get(t, key, receiver) {
    if (key === 'prototype') log.push('proto');
    return Reflect.get(t, key, receiver);
  },
});

class Base { constructor(log) { log.push('base'); } }
class Derived extends Base { constructor(log) { log.push('pre'); super(log); log.push('post'); } }
class Implicit extends Derived {}
class Outer extends Implicit { constructor(log) { log.push('outer-pre'); super(log); log.push('outer-post'); } }
class ViaArrow extends Base { constructor(log) { log.push('pre'); const run = () => super(log); run(); log.push('post'); } }
class ViaSpread extends Base { constructor(log) { log.push('pre'); super(...[log]); log.push('post'); } }
class FromArray extends Array { constructor(log) { log.push('pre'); super(); log.push('post'); } }
class FromMap extends Map { constructor(log) { log.push('pre'); super(); log.push('post'); } }
class FromError extends Error { constructor(log) { log.push('pre'); super(); log.push('post'); } }
class FromObject extends Object { constructor(log) { log.push('pre'); super(); log.push('post'); } }
class ReturnsObject extends Base { constructor() { return { replaced: true }; } }
class NoSuper extends Base { constructor() {} }
function Legacy(log) { log.push('legacy'); }

const expected = new Map([
  [Derived, 'pre,proto,base,post'],
  [Implicit, 'pre,proto,base,post'],
  [Outer, 'outer-pre,pre,proto,base,post,outer-post'],
  [ViaArrow, 'pre,proto,base,post'],
  [ViaSpread, 'pre,proto,base,post'],
  [FromArray, 'pre,proto,post'],
  [FromMap, 'pre,proto,post'],
  [FromError, 'pre,proto,post'],
  [FromObject, 'pre,proto,post'],
  [Legacy, 'proto,legacy'],
  [Base, 'proto,base'],
]);

for (let round = 0; round < 400; round++) {
  for (const [C, order] of expected) {
    let log = [];
    let newTarget = watched(log);
    let made = Reflect.construct(C, [log], newTarget);
    assert.strictEqual(log.join(), order, `${C.name} proxy new.target`);
    assert.strictEqual(Object.getPrototypeOf(made), newTarget.prototype, `${C.name} proxy new.target prototype`);

    log = [];
    newTarget = function () {};
    made = Reflect.construct(C, [log], newTarget);
    assert.strictEqual(Object.getPrototypeOf(made), newTarget.prototype, `${C.name} function new.target`);
    assert.strictEqual(log.join(), order.replace('proto,', '').replace(',proto', ''), `${C.name} function new.target order`);

    newTarget = class {};
    made = Reflect.construct(C, [[]], newTarget);
    assert.strictEqual(Object.getPrototypeOf(made), newTarget.prototype, `${C.name} class new.target`);

    made = Reflect.construct(C, [[]]);
    assert.strictEqual(Object.getPrototypeOf(made), C.prototype, `${C.name} default new.target`);

    log = [];
    const Proxied = new Proxy(C, {});
    made = new Proxied(log);
    assert.strictEqual(Object.getPrototypeOf(made), C.prototype, `${C.name} proxied class`);
    assert.strictEqual(log.join(), order.replace('proto,', '').replace(',proto', ''), `${C.name} proxied class order`);

    log = [];
    const Watched = watched(log, C);
    made = new Watched(log);
    assert.strictEqual(Object.getPrototypeOf(made), C.prototype, `${C.name} watched class`);
    assert.strictEqual(log.join(), order, `${C.name} watched class order`);
  }

  let log = [];
  assert.strictEqual(Reflect.construct(ReturnsObject, [], watched(log)).replaced, true);
  assert.deepStrictEqual(log, []);
  assert.throws(() => Reflect.construct(NoSuper, [], watched(log)), ReferenceError);
  assert.deepStrictEqual(log, []);

  const late = function () {};
  const replacement = {};
  class Reassigns extends Base { constructor(l) { late.prototype = replacement; super(l); } }
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Reassigns, [[]], late)), replacement);

  log = [];
  const boundTarget = watched(log);
  const bound = Reflect.construct(Derived.bind(null, log), [], boundTarget);
  assert.strictEqual(log.join(), 'pre,proto,base,post');
  assert.strictEqual(Object.getPrototypeOf(bound), boundTarget.prototype);

  const primitive = function () {};
  primitive.prototype = 5;
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Derived, [[]], primitive)), Object.prototype);
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Legacy, [[]], primitive)), Object.prototype);

  const builtin = function () {};
  builtin.prototype = Math.max;
  for (const C of [Derived, Implicit, Legacy, Base, FromArray, FromMap, Array, Map, Set, Object, Error, Date, RegExp, Uint8Array, Function, Promise]) {
    const args = C === Promise ? [() => {}] : C === Map || C === Set ? [] : [[]];
    const made = Reflect.construct(C, args, builtin);
    assert.strictEqual(Object.getPrototypeOf(made), Math.max, `${C.name} with a builtin function prototype`);
  }
}

{
  const target = Object.setPrototypeOf({}, Math.max);
  assert.strictEqual(Object.getPrototypeOf(target), Math.max);
  assert.strictEqual(Reflect.getPrototypeOf(target), Math.max);
  assert.strictEqual(target.__proto__, Math.max);
  assert.strictEqual(Object.getPrototypeOf(new Proxy(target, {})), Math.max);
  assert.strictEqual(Math.max.isPrototypeOf(target), true);
  assert.strictEqual(Math.min.isPrototypeOf(target), false);
  assert.strictEqual(target.name, 'max');

  const created = Object.create(Math.min);
  assert.strictEqual(Object.getPrototypeOf(created), Math.min);
  assert.strictEqual(created.length, 2);

  const literal = { __proto__: Math.abs };
  assert.strictEqual(Object.getPrototypeOf(literal), Math.abs);

  const assigned = {};
  assigned.__proto__ = Math.cos;
  assert.strictEqual(Object.getPrototypeOf(assigned), Math.cos);
  assert.strictEqual(Reflect.setPrototypeOf(assigned, Math.sin), true);
  assert.strictEqual(Object.getPrototypeOf(assigned), Math.sin);

  function Ctor() {}
  Ctor.prototype = Math.floor;
  for (let i = 0; i < 400; i++) {
    const made = new Ctor();
    assert.strictEqual(Object.getPrototypeOf(made), Math.floor);
    assert.strictEqual(made instanceof Ctor, true);
  }
  assert.strictEqual({} instanceof Ctor, false);
  assert.strictEqual(Object.create(Math.floor) instanceof Ctor, true);
  assert.strictEqual(Object.getPrototypeOf(Math.floor), Function.prototype);
}

console.log('ok');
