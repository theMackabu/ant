import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('Object Tests\n');

let obj = { a: 1, b: 2, c: 3 };
test('object property access', obj.a, 1);
test('object bracket access', obj['b'], 2);
test('object property count', Object.keys(obj).length, 3);

obj.d = 4;
test('add property', obj.d, 4);

obj.a = 10;
test('modify property', obj.a, 10);

delete obj.d;
test('delete property', obj.d, undefined);

test('in operator true', 'a' in obj, true);
test('in operator false', 'z' in obj, false);

testDeep('Object.keys', Object.keys({ x: 1, y: 2 }), ['x', 'y']);
testDeep('Object.values', Object.values({ x: 1, y: 2 }), [1, 2]);
testDeep('Object.entries', Object.entries({ x: 1 }), [['x', 1]]);

let merged = Object.assign({}, { a: 1 }, { b: 2 });
test('Object.assign a', merged.a, 1);
test('Object.assign b', merged.b, 2);

const ordered = {
  2: true,
  0: true,
  1: true,
  ' ': true,
  9: true,
  D: true,
  B: true,
  '-1': true
};
ordered.A = true;
ordered[3] = true;
'EFGHIJKLMNOPQRSTUVWXYZ'.split('').forEach(key => ordered[key] = true);
Object.defineProperty(ordered, 'C', { value: true, enumerable: true });
Object.defineProperty(ordered, '4', { value: true, enumerable: true });
delete ordered[2];
ordered[2] = true;

test('Object.getOwnPropertyNames order', Object.getOwnPropertyNames(ordered).join(''),
     '012349 DB-1AEFGHIJKLMNOPQRSTUVWXYZC');

const overflowNameOrder = { a: 1 };
overflowNameOrder['18446744073709551616'] = 2;
test('Object.getOwnPropertyNames overflow-like key order',
     Object.getOwnPropertyNames(overflowNameOrder).join('|'), 'a|18446744073709551616');

let assignOrder = '';
const assignTarget = {};
'012349 DBACEFGHIJKLMNOPQRST'.split('').concat(-1).forEach(key => {
  Object.defineProperty(assignTarget, key, {
    set() {
      assignOrder += key;
    }
  });
});
const assignSource = { 2: 2, 0: 0, 1: 1, ' ': ' ', 9: 9, D: 'D', B: 'B', '-1': '-1' };
Object.defineProperty(assignSource, 'A', { value: 'A', enumerable: true });
Object.defineProperty(assignSource, '3', { value: '3', enumerable: true });
Object.defineProperty(assignSource, 'C', { value: 'C', enumerable: true });
Object.defineProperty(assignSource, '4', { value: '4', enumerable: true });
delete assignSource[2];
assignSource[2] = true;
'EFGHIJKLMNOPQRST'.split('').forEach(key => assignSource[key] = key);
Object.assign(assignTarget, assignSource);
test('Object.assign property order', assignOrder, '012349 DB-1ACEFGHIJKLMNOPQRST');

const assignedArray = Object.assign({}, [10, 20]);
test('Object.assign dense array source 0', assignedArray[0], 10);
test('Object.assign dense array source 1', assignedArray[1], 20);

const sparseArraySource = [];
Object.defineProperty(sparseArraySource, '2', { value: 2, enumerable: true });
const assignedSparseArray = Object.assign({}, sparseArraySource);
test('Object.assign sparse array source index', assignedSparseArray[2], 2);

let overflowAssignOrder = '';
const overflowAssignTarget = {};
Object.defineProperty(overflowAssignTarget, 'a', { set() { overflowAssignOrder += 'a'; } });
Object.defineProperty(overflowAssignTarget, '18446744073709551616', {
  set() {
    overflowAssignOrder += 'h';
  }
});
const overflowAssignSource = { a: 1 };
overflowAssignSource['18446744073709551616'] = 2;
Object.assign(overflowAssignTarget, overflowAssignSource);
test('Object.assign overflow-like key order', overflowAssignOrder, 'ah');

let nested = { a: { b: { c: 1 } } };
test('nested access', nested.a.b.c, 1);

let computed = { ['key' + 1]: 'value' };
test('computed property', computed.key1, 'value');

let short = 'hello';
let shorthand = { short };
test('shorthand property', shorthand.short, 'hello');

let methodObj = {
  value: 42,
  getValue() {
    return this.value;
  }
};
test('method shorthand', methodObj.getValue(), 42);

let source = { a: 1, b: 2 };
let spread = { ...source, c: 3 };
test('spread a', spread.a, 1);
test('spread c', spread.c, 3);

let { a, b } = { a: 1, b: 2, c: 3 };
test('destructuring a', a, 1);
test('destructuring b', b, 2);

let { x: renamed } = { x: 5 };
test('destructuring rename', renamed, 5);

let { y = 10 } = {};
test('destructuring default', y, 10);

test('hasOwnProperty true', { a: 1 }.hasOwnProperty('a'), true);
test('hasOwnProperty false', { a: 1 }.hasOwnProperty('b'), false);

let frozen = Object.freeze({ a: 1 });
test('Object.isFrozen', Object.isFrozen(frozen), true);

let sealed = Object.seal({ a: 1 });
test('Object.isSealed', Object.isSealed(sealed), true);

let proto = { inherited: true };
let child = Object.create(proto);
test('Object.create inherits', child.inherited, true);

test('Object.getPrototypeOf', Object.getPrototypeOf([]) === Array.prototype, true);

testDeep(
  'Object.fromEntries',
  Object.fromEntries([
    ['a', 1],
    ['b', 2]
  ]),
  { a: 1, b: 2 }
);

let descriptorSource = {};
const descriptorSymbol = Symbol('descriptor');
Object.defineProperty(descriptorSource, 'hidden', {
  value: 42,
  enumerable: false,
  writable: false,
  configurable: false
});
Object.defineProperty(descriptorSource, 'computed', {
  get() {
    return 7;
  },
  enumerable: true,
  configurable: true
});
descriptorSource.visible = 'yes';
descriptorSource[descriptorSymbol] = 'symbol value';

const descriptors = Object.getOwnPropertyDescriptors(descriptorSource);
test('Object.getOwnPropertyDescriptors exists', typeof Object.getOwnPropertyDescriptors, 'function');
test('descriptors data value', descriptors.visible.value, 'yes');
test('descriptors hidden value', descriptors.hidden.value, 42);
test('descriptors hidden enumerable', descriptors.hidden.enumerable, false);
test('descriptors hidden writable', descriptors.hidden.writable, false);
test('descriptors hidden configurable', descriptors.hidden.configurable, false);
test('descriptors accessor getter', typeof descriptors.computed.get, 'function');
test('descriptors accessor enumerable', descriptors.computed.enumerable, true);
test('descriptors symbol value', descriptors[descriptorSymbol].value, 'symbol value');

const arrayDescriptors = Object.getOwnPropertyDescriptors(['item']);
test('array descriptor index value', arrayDescriptors[0].value, 'item');
test('array descriptor length value', arrayDescriptors.length.value, 1);
const proxyUndefinedDescriptor = new Proxy({ a: 1 }, {
  getOwnPropertyDescriptor() {}
});
test('Object.getOwnPropertyDescriptors skips undefined proxy descriptors', Object.getOwnPropertyDescriptors(proxyUndefinedDescriptor).hasOwnProperty('a'), false);
testThrows('Object.getOwnPropertyDescriptors throws without argument', () => Object.getOwnPropertyDescriptors());
testThrows('Object.getOwnPropertyDescriptors throws on null', () => Object.getOwnPropertyDescriptors(null));
test('Object.getOwnPropertyDescriptors boxes number primitive', typeof Object.getOwnPropertyDescriptors(1), 'object');

summary();
