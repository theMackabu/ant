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
