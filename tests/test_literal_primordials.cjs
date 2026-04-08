function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(msg + ' (expected ' + expected + ', got ' + actual + ')');
  }
}

const BuiltinObject = Object;
const BuiltinArray = Array;
const primordialObjectProto = BuiltinObject.getPrototypeOf({});
const primordialArrayProto = BuiltinObject.getPrototypeOf([]);
const originalObjectCtorProto = BuiltinObject.prototype;
const originalArrayCtorProto = BuiltinArray.prototype;

function restore() {
  globalThis.Object = BuiltinObject;
  globalThis.Array = BuiltinArray;
  BuiltinObject.prototype = originalObjectCtorProto;
  BuiltinArray.prototype = originalArrayCtorProto;
}

function tryRedefineCtorPrototype(ctor, proto) {
  try {
    BuiltinObject.defineProperty(ctor, 'prototype', {
      value: proto,
      writable: true
    });
    return true;
  } catch (_) {
    return false;
  }
}

console.log('literal primordial prototype regression');

try {
  console.log('\nTest 1: object literal ignores Object.prototype assignment');
  {
    const altProto = { mark: 'object-assign' };
    BuiltinObject.prototype = altProto;
    const obj = {};
    assert(BuiltinObject.getPrototypeOf(obj) === primordialObjectProto, 'object literal should keep primordial Object prototype');
    assertEq(obj.mark, undefined, 'object literal should ignore assigned ctor prototype');
  }
  restore();
  console.log('PASS');

  console.log('\nTest 2: object literal ignores global Object rebinding');
  {
    function AltObject() {}
    AltObject.prototype = { mark: 'object-global' };
    globalThis.Object = AltObject;
    assertEq(globalThis.Object.prototype.mark, 'object-global', 'global Object rebinding should be visible from JS');
    const obj = {};
    assert(
      BuiltinObject.getPrototypeOf(obj) === primordialObjectProto,
      'object literal should keep primordial Object prototype after global rebinding'
    );
    assertEq(obj.mark, undefined, 'object literal should ignore rebound global Object');
  }
  restore();
  console.log('PASS');

  console.log('\nTest 3: object literal ignores Object.defineProperty on ctor prototype');
  {
    const altProto = { mark: 'object-define' };
    if (!tryRedefineCtorPrototype(BuiltinObject, altProto)) {
      console.log('SKIP');
    } else {
    assertEq(BuiltinObject.prototype.mark, 'object-define', 'defineProperty should update Object.prototype property');
    const obj = {};
    assert(
      BuiltinObject.getPrototypeOf(obj) === primordialObjectProto,
      'object literal should keep primordial Object prototype after defineProperty'
    );
    assertEq(obj.mark, undefined, 'object literal should ignore defineProperty-updated ctor prototype');
    }
  }
  restore();
  console.log('PASS');

  console.log('\nTest 4: array literal ignores Array.prototype assignment');
  {
    const altProto = [];
    altProto.mark = 'array-assign';
    BuiltinArray.prototype = altProto;
    const arr = [];
    assert(BuiltinObject.getPrototypeOf(arr) === primordialArrayProto, 'array literal should keep primordial Array prototype');
    assertEq(arr.mark, undefined, 'array literal should ignore assigned ctor prototype');
  }
  restore();
  console.log('PASS');

  console.log('\nTest 5: array literal ignores global Array rebinding');
  {
    function AltArray() {}
    AltArray.prototype = { mark: 'array-global' };
    globalThis.Array = AltArray;
    assertEq(globalThis.Array.prototype.mark, 'array-global', 'global Array rebinding should be visible from JS');
    const arr = [];
    assert(BuiltinObject.getPrototypeOf(arr) === primordialArrayProto, 'array literal should keep primordial Array prototype after global rebinding');
    assertEq(arr.mark, undefined, 'array literal should ignore rebound global Array');
  }
  restore();
  console.log('PASS');

  console.log('\nTest 6: array literal ignores Object.defineProperty on ctor prototype');
  {
    const altProto = [];
    altProto.mark = 'array-define';
    if (!tryRedefineCtorPrototype(BuiltinArray, altProto)) {
      console.log('SKIP');
    } else {
      assertEq(BuiltinArray.prototype.mark, 'array-define', 'defineProperty should update Array.prototype property');
      const arr = [];
      assert(BuiltinObject.getPrototypeOf(arr) === primordialArrayProto, 'array literal should keep primordial Array prototype after defineProperty');
      assertEq(arr.mark, undefined, 'array literal should ignore defineProperty-updated ctor prototype');
    }
  }
  restore();
  console.log('PASS');

  console.log('\nAll literal primordial prototype tests passed');
} finally {
  restore();
}
