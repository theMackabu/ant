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

console.log('ok');
