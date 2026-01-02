function testCode() {
  // __proto__ in object literals - basic support
  if (!({ __proto__: [] } instanceof Array) || { __proto__: null } instanceof Object) {
    return false;
  }

  // __proto__ in object literals - multiple __proto__ is an error
  try {
    eval('({ __proto__ : [], __proto__: {} })');
    return false;
  } catch (e) {}

  // __proto__ in object literals - not a computed property
  var a = '__proto__';
  if ({ [a]: [] } instanceof Array) {
    return false;
  }

  // __proto__ in object literals - not a shorthand method
  if ({ __proto__() {} } instanceof Function) {
    return false;
  }

  // __proto__ in object literals - not a shorthand property
  var __proto__ = [];
  if ({ __proto__ } instanceof Array) {
    return false;
  }

  // Object.prototype.__proto__ - get prototype
  var A = function () {};
  if (new A().__proto__ !== A.prototype) {
    return false;
  }

  // Object.prototype.__proto__ - set prototype
  var o = {};
  o.__proto__ = Array.prototype;
  if (!(o instanceof Array)) {
    return false;
  }

  // Object.prototype.__proto__ - absent from Object.create(null)
  var o2 = Object.create(null),
    p = {};
  o2.__proto__ = p;
  if (Object.getPrototypeOf(o2) === p) {
    return false;
  }

  // Object.prototype.__proto__ - correct property descriptor
  var desc = Object.getOwnPropertyDescriptor(Object.prototype, '__proto__');
  if (!(desc && 'get' in desc && 'set' in desc && desc.configurable && !desc.enumerable)) {
    return false;
  }

  // Object.prototype.__proto__ - present in hasOwnProperty()
  if (!Object.prototype.hasOwnProperty('__proto__')) {
    return false;
  }

  // Object.prototype.__proto__ - present in Object.getOwnPropertyNames()
  if (Object.getOwnPropertyNames(Object.prototype).indexOf('__proto__') === -1) {
    return false;
  }

  return true;
}

try {
  if (testCode()) {
    console.log('kangax-es6/annex-b.__proto__.js: OK');
  } else {
    console.log('kangax-es6/annex-b.__proto__.js: failed');
  }
} catch (e) {
  console.log('kangax-es6/annex-b.__proto__.js: exception: ' + e);
}
