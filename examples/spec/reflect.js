import { test, testDeep, summary } from './helpers.js';

console.log('Reflect Tests\n');

const obj = { a: 1, b: 2 };

test('Reflect.get', Reflect.get(obj, 'a'), 1);

Reflect.set(obj, 'c', 3);
test('Reflect.set', obj.c, 3);

test('Reflect.has true', Reflect.has(obj, 'a'), true);
test('Reflect.has false', Reflect.has(obj, 'z'), false);

Reflect.deleteProperty(obj, 'c');
test('Reflect.deleteProperty', obj.c, undefined);

testDeep('Reflect.ownKeys', Reflect.ownKeys({ x: 1, y: 2 }), ['x', 'y']);
testDeep('Reflect.ownKeys array', Reflect.ownKeys([1]), ['0', 'length']);

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
test('Reflect.ownKeys string order', Reflect.ownKeys(ordered).join(''), '012349 DB-1AEFGHIJKLMNOPQRSTUVWXYZC');

const sparseArray = [];
Object.defineProperty(sparseArray, '2', { value: 2, enumerable: true });
testDeep('Reflect.ownKeys sparse array', Reflect.ownKeys(sparseArray), ['2', 'length']);

const overflowKeyOrder = { a: 1 };
overflowKeyOrder['18446744073709551616'] = 2;
test('Reflect.ownKeys overflow-like key order',
     Reflect.ownKeys(overflowKeyOrder).join('|'), 'a|18446744073709551616');

const sym1 = Symbol();
const sym2 = Symbol();
const sym3 = Symbol();
const symbolOrder = { 1: true, A: true };
symbolOrder.B = true;
symbolOrder[sym1] = true;
symbolOrder[2] = true;
symbolOrder[sym2] = true;
Object.defineProperty(symbolOrder, 'C', { value: true, enumerable: true });
Object.defineProperty(symbolOrder, sym3, { value: true, enumerable: true });
Object.defineProperty(symbolOrder, 'D', { value: true, enumerable: true });
const symbolKeys = Reflect.ownKeys(symbolOrder);
test('Reflect.ownKeys symbol order 1', symbolKeys[symbolKeys.length - 3], sym1);
test('Reflect.ownKeys symbol order 2', symbolKeys[symbolKeys.length - 2], sym2);
test('Reflect.ownKeys symbol order 3', symbolKeys[symbolKeys.length - 1], sym3);

function Point(x, y) {
  this.x = x;
  this.y = y;
}
const p = Reflect.construct(Point, [3, 4]);
test('Reflect.construct x', p.x, 3);
test('Reflect.construct y', p.y, 4);

function greet(greeting) {
  return greeting + ' ' + this.name;
}
test('Reflect.apply', Reflect.apply(greet, { name: 'World' }, ['Hello']), 'Hello World');

const desc = Reflect.getOwnPropertyDescriptor(obj, 'a');
test('Reflect.getOwnPropertyDescriptor value', desc.value, 1);

Reflect.defineProperty(obj, 'd', { value: 4, writable: true });
test('Reflect.defineProperty', obj.d, 4);

test('Reflect.getPrototypeOf array', Reflect.getPrototypeOf([]) === Array.prototype, true);

const newProto = { inherited: true };
const child = {};
Reflect.setPrototypeOf(child, newProto);
test('Reflect.setPrototypeOf', child.inherited, true);

test('Reflect.isExtensible', Reflect.isExtensible({}), true);

const frozen = Object.freeze({});
test('Reflect.isExtensible frozen', Reflect.isExtensible(frozen), false);

summary();
