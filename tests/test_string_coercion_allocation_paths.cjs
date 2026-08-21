function assert(condition, message) {
  if (!condition) {
    console.log('FAIL:', message);
    process.exit(1);
  }
}

function equal(actual, expected, message) {
  assert(actual === expected, `${message}: expected ${expected}, got ${actual}`);
}

function throws(fn, message) {
  let didThrow = false;
  try { fn(); } catch (_) { didThrow = true; }
  assert(didThrow, message);
}

const longDigits = '9'.repeat(100);
const longBigInt = BigInt(longDigits);
const keyed = {};
Object.defineProperty(keyed, longBigInt, {
  value: 17,
  enumerable: true,
  configurable: true,
});

equal(Object.keys(keyed)[0], longDigits, 'defineProperty preserves a long BigInt key');
equal(Object.getOwnPropertyDescriptor(keyed, longBigInt).value, 17, 'descriptor uses the full key');
equal(
  Object.getOwnPropertyDescriptor({ undefined: 31 }).value,
  31,
  'descriptor treats an omitted key as undefined',
);
assert(keyed.hasOwnProperty(longBigInt), 'hasOwnProperty uses the full key');
assert(keyed.propertyIsEnumerable(longBigInt), 'propertyIsEnumerable uses the full key');
assert(
  ({ undefined: 1 }).hasOwnProperty(),
  'hasOwnProperty treats an omitted key as undefined',
);
assert(
  Object.hasOwn({ undefined: 1 }),
  'Object.hasOwn treats an omitted key as undefined',
);

const fromEntries = Object.fromEntries([[longBigInt, 23]]);
equal(Object.keys(fromEntries)[0], longDigits, 'fromEntries preserves a long BigInt key');
equal(fromEntries[longDigits], 23, 'fromEntries stores the expected value');
equal(
  Object.fromEntries([[undefined, 29]]).undefined,
  29,
  'fromEntries accepts undefined as a property key',
);

let proxyDefineKey;
Object.defineProperty(new Proxy({}, {
  defineProperty(_target, key) {
    proxyDefineKey = key;
    return true;
  },
}), 12345678901234567890n, { value: 1 });
equal(proxyDefineKey, '12345678901234567890', 'proxy defineProperty receives a string key');

const keyCoercionOrder = [];
const longObjectKey = {
  [Symbol.toPrimitive](hint) {
    keyCoercionOrder.push(hint);
    return 'k'.repeat(400);
  },
};
Object.defineProperty(keyed, longObjectKey, { value: 31, enumerable: true });
equal(keyCoercionOrder.join(','), 'string', 'property key uses the string primitive hint once');
equal(keyed['k'.repeat(400)], 31, 'object property key is not truncated');

const symbolKey = Symbol('coerced-key');
const symbolKeyObject = { [Symbol.toPrimitive]() { return symbolKey; } };
Object.defineProperty(keyed, symbolKeyObject, { value: 37, enumerable: true });
equal(keyed[symbolKey], 37, 'ToPropertyKey preserves a returned symbol');
assert(keyed.hasOwnProperty(symbolKeyObject), 'hasOwnProperty preserves a coerced symbol');

const coercionError = { [Symbol.toPrimitive]() { throw new Error('key coercion'); } };
throws(
  () => Object.prototype.hasOwnProperty.call(keyed, coercionError),
  'property-key coercion errors propagate',
);

const longText = 'x'.repeat(1000);
equal([longText].toLocaleString(), longText, 'array toLocaleString preserves long strings');

let localeCalls = 0;
const options = { style: 'unit' };
const localizable = {
  toLocaleString(locales, receivedOptions) {
    localeCalls++;
    equal(locales, 'en-US', 'toLocaleString forwards locales');
    assert(receivedOptions === options, 'toLocaleString forwards options');
    return longText;
  },
};
equal(
  [localizable, null, undefined].toLocaleString('en-US', options),
  `${longText},,`,
  'array toLocaleString invokes elements and preserves empty entries',
);
equal(localeCalls, 1, 'element toLocaleString is invoked once');
equal([, 'tail'].toLocaleString(), ',tail', 'array toLocaleString preserves holes');
throws(() => [{ toLocaleString: 1 }].toLocaleString(), 'non-callable element method throws');

let objectLocaleInspectCalls = 0;
const localeUndefined = {
  toString() { return undefined; },
  [Symbol.inspect]() {
    objectLocaleInspectCalls++;
    return 'inspected'.repeat(100);
  },
};
equal(
  Object.prototype.toLocaleString.call(localeUndefined),
  undefined,
  'Object toLocaleString returns undefined without coercing it',
);
equal(objectLocaleInspectCalls, 0, 'Object toLocaleString does not inspect its receiver');

const returnedFunction = Array.prototype.join;
assert(
  Object.prototype.toLocaleString.call({ toString() { return returnedFunction; } }) === returnedFunction,
  'Object toLocaleString returns the exact method result',
);

let stringInspectCalls = 0;
const stringUndefined = {
  toString() { return undefined; },
  [Symbol.inspect]() {
    stringInspectCalls++;
    return 'wrong'.repeat(200);
  },
};
equal(String(stringUndefined), 'undefined', 'String uses the primitive returned by toString');
equal(stringInspectCalls, 0, 'String coercion does not use Symbol.inspect');
throws(
  () => String({ toString() { return returnedFunction; } }),
  'String rejects an object result when valueOf also returns an object',
);

let proxyStringCalls = 0;
const proxyToString = new Proxy(function () {
  proxyStringCalls++;
  return 'proxy string';
}, {});
equal(String({ toString: proxyToString }), 'proxy string', 'String invokes a callable proxy method');
equal(proxyStringCalls, 1, 'callable proxy toString is invoked once');

let ordinaryFallbackCalls = 0;
const exoticUndefined = {
  [Symbol.toPrimitive]() { return undefined; },
  toString() {
    ordinaryFallbackCalls++;
    return 'wrong';
  },
};
equal(String(exoticUndefined), 'undefined', 'undefined is a valid exotic primitive result');
equal(ordinaryFallbackCalls, 0, 'exotic primitive conversion does not fall through');

const numberCoercionOrder = [];
equal(
  Number({
    [Symbol.toPrimitive](hint) {
      numberCoercionOrder.push(hint);
      return 42;
    },
  }),
  42,
  'Number uses exotic primitive conversion',
);
equal(numberCoercionOrder.join(','), 'number', 'Number supplies the number primitive hint once');

let proxyValueOfCalls = 0;
const proxyValueOf = new Proxy(function () {
  proxyValueOfCalls++;
  return 73;
}, {});
equal(Number({ valueOf: proxyValueOf }), 73, 'Number invokes a callable proxy valueOf');
equal(proxyValueOfCalls, 1, 'callable proxy valueOf is invoked once');

const cachedOrdinaryNumber = { valueOf() { return 5; } };
equal(Number(cachedOrdinaryNumber), 5, 'ordinary number conversion before exotic method');
cachedOrdinaryNumber[Symbol.toPrimitive] = () => 7;
equal(Number(cachedOrdinaryNumber), 7, 'adding an exotic method invalidates primitive lookup');

const cachedPrototypeNumber = { valueOf() { return 11; } };
equal(Number(cachedPrototypeNumber), 11, 'ordinary number conversion before prototype change');
const numericPrototype = {
  [Symbol.toPrimitive]() { return 13; },
};
Object.setPrototypeOf(cachedPrototypeNumber, numericPrototype);
equal(Number(cachedPrototypeNumber), 13, 'changing the prototype invalidates primitive lookup');

const cachedValueMutation = {
  valueOf() { return 17; },
  toString() { return '23'; },
};
equal(Number(cachedValueMutation), 17, 'cached own valueOf before replacement');
cachedValueMutation.valueOf = () => 19;
equal(Number(cachedValueMutation), 19, 'cached own valueOf loads its current value');
cachedValueMutation.valueOf = 1;
equal(Number(cachedValueMutation), 23, 'non-callable cached valueOf falls through to toString');

const originalNumberToLocaleString = Number.prototype.toLocaleString;
let numberLocaleCalls = 0;
Number.prototype.toLocaleString = function (locales) {
  numberLocaleCalls++;
  return `${locales}:${this}`;
};
equal(
  [12, 34].toLocaleString('test'),
  'test:12,test:34',
  'array toLocaleString observes an overridden number method',
);
equal(numberLocaleCalls, 2, 'overridden number locale method is called per element');
Number.prototype.toLocaleString = originalNumberToLocaleString;

let proxyLocaleCalls = 0;
Number.prototype.toLocaleString = new Proxy(function (locales) {
  proxyLocaleCalls++;
  return `${locales}:${this}`;
}, {});
equal(
  [12, 34].toLocaleString('proxy'),
  'proxy:12,proxy:34',
  'array toLocaleString invokes callable proxy methods',
);
equal(proxyLocaleCalls, 2, 'callable proxy locale method is called per element');
Number.prototype.toLocaleString = originalNumberToLocaleString;

const manyLocaleNumbers = Array(100).fill(123456789);
equal(
  manyLocaleNumbers.toLocaleString(),
  `123,456,789${',123,456,789'.repeat(99)}`,
  'numeric locale fast path grows past stack storage',
);

const concatOrder = [];
const concatResult = 'head'.concat(
  { toString() { concatOrder.push('a'); return 'A'.repeat(300); } },
  { toString() { concatOrder.push('b'); return 'B'.repeat(300); } },
  ...Array(10).fill('!'),
);
equal(concatOrder.join(','), 'a,b', 'concat coerces arguments once in order');
equal(concatResult, `head${'A'.repeat(300)}${'B'.repeat(300)}${'!'.repeat(10)}`, 'concat result');

const joinOrder = [];
const separator = { toString() { joinOrder.push('separator'); return '::'; } };
const joined = [
  { toString() { joinOrder.push('first'); return 'a'.repeat(300); } },
  42,
  true,
].join(separator);
equal(joinOrder.join(','), 'separator,first', 'join coerces separator and elements once in order');
equal(joined, `${'a'.repeat(300)}::42::true`, 'join result');

const genericJoinOrder = [];
const genericJoinReceiver = new Proxy({ 0: 'value', length: 1 }, {
  get(target, key, receiver) {
    genericJoinOrder.push(`get:${String(key)}`);
    return Reflect.get(target, key, receiver);
  },
});
const genericJoinSeparator = {
  toString() {
    genericJoinOrder.push('separator');
    return ':';
  },
};
equal(
  Array.prototype.join.call(genericJoinReceiver, genericJoinSeparator),
  'value',
  'generic join result',
);
equal(
  genericJoinOrder.join(','),
  'get:length,separator,get:0',
  'join reads length before coercing the separator',
);
equal(
  Array(300).fill('abc').join('::'),
  `abc${'::abc'.repeat(299)}`,
  'packed primitive join writes a long result directly',
);
throws(() => [Symbol('x')].join(','), 'join rejects implicit Symbol conversion');

const originalObjectToLocaleString = Object.prototype.toLocaleString;
let stringLocaleOverrideCalls = 0;
Object.prototype.toLocaleString = function () {
  stringLocaleOverrideCalls++;
  return `localized:${this}`;
};
equal(
  ['a', 'b'].toLocaleString(),
  'localized:a,localized:b',
  'string array toLocaleString observes an overridden inherited method',
);
equal(stringLocaleOverrideCalls, 2, 'string locale override is called per element');
Object.prototype.toLocaleString = originalObjectToLocaleString;

const originalStringToString = String.prototype.toString;
let stringToStringOverrideCalls = 0;
String.prototype.toString = function () {
  stringToStringOverrideCalls++;
  return `string:${originalStringToString.call(this)}`;
};
equal(
  ['a', 'b'].toLocaleString(),
  'string:a,string:b',
  'string array toLocaleString observes an overridden toString method',
);
equal(stringToStringOverrideCalls, 2, 'string toString override is called per element');
String.prototype.toString = originalStringToString;

const objectLocaleDescriptor = Object.getOwnPropertyDescriptor(
  Object.prototype,
  'toLocaleString',
);
let objectLocaleGetterCalls = 0;
Object.defineProperty(Object.prototype, 'toLocaleString', {
  configurable: true,
  get() {
    objectLocaleGetterCalls++;
    return function () { return 'object-accessor'; };
  },
});
equal(
  ['a', 'b'].toLocaleString(),
  'object-accessor,object-accessor',
  'string array toLocaleString invokes an inherited primitive accessor',
);
equal(objectLocaleGetterCalls, 2, 'inherited locale accessor is invoked per element');
Object.defineProperty(Object.prototype, 'toLocaleString', objectLocaleDescriptor);

const stringToStringDescriptor = Object.getOwnPropertyDescriptor(
  String.prototype,
  'toString',
);
let stringToStringGetterCalls = 0;
Object.defineProperty(String.prototype, 'toString', {
  configurable: true,
  get() {
    stringToStringGetterCalls++;
    return function () { return 'string-accessor'; };
  },
});
equal(
  ['a', 'b'].toLocaleString(),
  'string-accessor,string-accessor',
  'Object toLocaleString invokes a primitive String prototype accessor',
);
equal(stringToStringGetterCalls, 2, 'String accessor is invoked per element');
Object.defineProperty(String.prototype, 'toString', stringToStringDescriptor);

const primitiveAccessorKey = '__ant_primitive_accessor__';
Object.defineProperty(BigInt.prototype, primitiveAccessorKey, {
  configurable: true,
  get: function () {
    'use strict';
    return this;
  },
});
const accessorSymbol = Symbol('accessor receiver');
Object.defineProperty(Symbol.prototype, primitiveAccessorKey, {
  configurable: true,
  get: function () {
    'use strict';
    return this;
  },
});
assert(
  (17n)[primitiveAccessorKey] === 17n,
  'BigInt prototype accessors receive the primitive receiver',
);
assert(
  accessorSymbol[primitiveAccessorKey] === accessorSymbol,
  'Symbol prototype accessors receive the primitive receiver',
);
delete BigInt.prototype[primitiveAccessorKey];
delete Symbol.prototype[primitiveAccessorKey];

Object.defineProperty(Object.prototype, primitiveAccessorKey, {
  configurable: true,
  value: 42,
});
Object.defineProperty(String.prototype, primitiveAccessorKey, {
  configurable: true,
  set(_) {},
});
equal(
  'value'[primitiveAccessorKey],
  undefined,
  'a setter-only primitive property shadows older prototype data',
);
delete String.prototype[primitiveAccessorKey];
delete Object.prototype[primitiveAccessorKey];

equal(
  ['a\0b', 'c'].toLocaleString(),
  'a\0b,c',
  'string array locale fast path preserves embedded NUL bytes',
);

const charCodes = Array(300).fill(65);
equal(String.fromCharCode(...charCodes), 'A'.repeat(300), 'fromCharCode grows past stack storage');
const codePoints = Array(100).fill(0x1F600);
equal(String.fromCodePoint(...codePoints), '😀'.repeat(100), 'fromCodePoint grows past stack storage');
throws(() => String.fromCodePoint(0x110000), 'fromCodePoint preserves range errors');

equal(String(123456789n), '123456789', 'small BigInt conversion');
const hugeBigIntDigits = '8'.repeat(500);
equal(String(BigInt(hugeBigIntDigits)), hugeBigIntDigits, 'large BigInt conversion');

const regexpOrder = [];
const regexpLike = {
  get source() { regexpOrder.push('source'); return 'r'.repeat(500); },
  get flags() { regexpOrder.push('flags'); return 'gi'; },
};
equal(
  RegExp.prototype.toString.call(regexpLike),
  `/${'r'.repeat(500)}/gi`,
  'RegExp toString writes directly into the final string',
);
equal(regexpOrder.join(','), 'source,flags', 'RegExp toString reads properties once in order');
equal(RegExp.escape('abc-def/ghi'), '\\x61bc\\x2ddef\\/ghi', 'RegExp.escape output');
equal(RegExp.escape('\t\n\v\f\r '), '\\t\\n\\v\\f\\r\\x20', 'RegExp.escape short whitespace escapes');
equal(RegExp.escape('a'.repeat(500)).length, 503, 'RegExp.escape handles long input');

for (let i = 0; i < 2000; i++) {
  equal('x'.concat(i, '-', true), `x${i}-true`, 'repeated concat');
  equal([i, false, 'z'].join(':'), `${i}:false:z`, 'repeated join');
}

console.log('PASS');
