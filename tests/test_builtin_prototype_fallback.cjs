'use strict';
const assert = require('node:assert');

{
  function G() {}
  function NoProto() {}
  NoProto.prototype = 1;
  const proxied = () => new Proxy(function () {}, { get(t, k) { return k === 'prototype' ? 5 : t[k]; } });
  for (const C of [Map, Set, Array, Error, Date, RegExp, Uint8Array, ArrayBuffer, WeakMap, Boolean, Number, String, Function]) {
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(C, [], new Proxy(G, {}))), G.prototype, C.name);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(C, [], proxied())), C.prototype, C.name);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(C, [], NoProto)), C.prototype, C.name);
  }
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(function () {}, [], proxied())), Object.prototype);
}

{
  class Base {}
  function Target() {}
  Target.prototype = 1;
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Base, [], Target)), Object.prototype);
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Map, [], Target)), Map.prototype);
}

{
  class Holder {}
  const bound = Map.bind(null);
  Object.setPrototypeOf(bound, Holder);
  const accessor = Set.bind(null);
  let reads = 0;
  Object.defineProperty(accessor, 'prototype', { get() { reads++; return Holder.prototype; } });
  const throwing = Map.bind(null);
  Object.defineProperty(throwing, 'prototype', { get() { throw new RangeError('prototype'); } });
  for (let round = 0; round < 200; round++) {
    const map = Reflect.construct(Map, [], bound);
    assert.strictEqual(Object.getPrototypeOf(map), Holder.prototype);
    assert.strictEqual(Map.prototype.set.call(map, 1, 2), map);
    reads = 0;
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Set, [], accessor)), Holder.prototype);
    assert.strictEqual(reads, 1);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Uint8Array, [2], bound)), Holder.prototype);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Error, ['m'], bound)), Holder.prototype);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Array, [], bound)), Holder.prototype);
    assert.throws(() => Reflect.construct(Map, [], throwing), RangeError);
    assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Map, [], Map.bind(null))), Map.prototype);
  }
}

console.log('ok');
