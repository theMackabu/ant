function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const BooleanNewTarget = function BooleanNewTarget() {};
const booleanObject = Reflect.construct(Boolean, [], BooleanNewTarget);

assert(
  Boolean.prototype.valueOf.call(booleanObject) === false,
  'Reflect.construct(Boolean, [], newTarget) should initialize [[BooleanData]]',
);
assert(
  Object.getPrototypeOf(booleanObject) === BooleanNewTarget.prototype,
  'Reflect.construct(Boolean, [], newTarget) should use newTarget.prototype',
);

const NumberNewTarget = function NumberNewTarget() {};
const numberObject = Reflect.construct(Number, [42], NumberNewTarget);

assert(
  Number.prototype.valueOf.call(numberObject) === 42,
  'Reflect.construct(Number, [], newTarget) should initialize [[NumberData]]',
);
assert(
  Object.getPrototypeOf(numberObject) === NumberNewTarget.prototype,
  'Reflect.construct(Number, [], newTarget) should use newTarget.prototype',
);

const StringNewTarget = function StringNewTarget() {};
const stringObject = Reflect.construct(String, ['ant'], StringNewTarget);

assert(
  String.prototype.valueOf.call(stringObject) === 'ant',
  'Reflect.construct(String, [], newTarget) should initialize [[StringData]]',
);
assert(
  Object.getPrototypeOf(stringObject) === StringNewTarget.prototype,
  'Reflect.construct(String, [], newTarget) should use newTarget.prototype',
);

function supportsReflectConstructNewTarget() {
  try {
    return !Boolean.prototype.valueOf.call(Reflect.construct(Boolean, [], function() {}));
  } catch (_) {
    return false;
  }
}

function inherit(Sub, Super) {
  Sub.prototype = Object.create(Super && Super.prototype, {
    constructor: { value: Sub, writable: true, configurable: true },
  });
  Object.defineProperty(Sub, 'prototype', { writable: false });
  if (Super) Object.setPrototypeOf(Sub, Super);
}

function getPrototypeOf(value) {
  return Object.getPrototypeOf.bind()(value);
}

function constructSuper(self, Constructor, args) {
  const Super = getPrototypeOf(Constructor);
  return Reflect.construct(Super, args || [], getPrototypeOf(self).constructor);
}

const SafeWeakMap = (function(Super) {
  function SafeWeakMap(iterable) {
    return constructSuper(this, SafeWeakMap, [iterable]);
  }
  inherit(SafeWeakMap, Super);
  Object.setPrototypeOf(SafeWeakMap.prototype, null);
  Object.freeze(SafeWeakMap.prototype);
  Object.freeze(SafeWeakMap);
  return SafeWeakMap;
})(WeakMap);

assert(supportsReflectConstructNewTarget(), 'Reflect.construct newTarget probe should pass');

const safeWeakMap = new SafeWeakMap();
assert(safeWeakMap instanceof SafeWeakMap, 'derived WeakMap should use derived prototype');

console.log('reflect:construct-newtarget-wrappers:ok');
