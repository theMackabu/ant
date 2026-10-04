import { test, testDeep, testThrows, summary } from './helpers.js';

console.log('Iterator Tests\n');

const arr = [1, 2, 3];
const iter = arr[Symbol.iterator]();
test('array iterator first', iter.next().value, 1);
test('array iterator second', iter.next().value, 2);
test('array iterator third', iter.next().value, 3);
test('array iterator done', iter.next().done, true);

const str = 'abc';
const strIter = str[Symbol.iterator]();
test('string iterator first', strIter.next().value, 'a');
test('string iterator second', strIter.next().value, 'b');
test('string iterator third', strIter.next().value, 'c');
test('string iterator done', strIter.next().done, true);

const map = new Map([
  ['a', 1],
  ['b', 2]
]);
const mapIter = map.entries();
testDeep('map entries first', mapIter.next().value, ['a', 1]);
testDeep('map entries second', mapIter.next().value, ['b', 2]);

const set = new Set([1, 2, 3]);
const setIter = set.values();
test('set values first', setIter.next().value, 1);
test('set values second', setIter.next().value, 2);

let forOfSum = 0;
for (const n of [1, 2, 3]) {
  forOfSum += n;
}
test('for-of array', forOfSum, 6);

let forOfStr = '';
for (const c of 'hi') {
  forOfStr += c;
}
test('for-of string', forOfStr, 'hi');

const custom = {
  data: [10, 20, 30],
  [Symbol.iterator]() {
    let i = 0;
    return {
      next: () => {
        if (i < this.data.length) {
          return { value: this.data[i++], done: false };
        }
        return { done: true };
      }
    };
  }
};

let customSum = 0;
for (const n of custom) {
  customSum += n;
}
test('custom iterator', customSum, 60);

let capturedNestedForOf = 0;
const capturedFns = {};
for (const { values = [] } of [{ values: ['a', 'bb'] }]) {
  for (const value of values) {
    capturedFns[value] = () => value.length;
    capturedNestedForOf++;
  }
}
test('captured nested for-of count', capturedNestedForOf, 2);
test('captured nested for-of closure', capturedFns.bb(), 2);

const stringIterator = String.prototype[Symbol.iterator];
test('string iterator coerces a number receiver', [...stringIterator.call(123)].join(','), '1,2,3');
test('string iterator coerces an object receiver', [...stringIterator.call({ toString() { return 'ob'; } })].join(','), 'o,b');
testThrows('string iterator rejects null', () => stringIterator.call(null));
testThrows('string iterator rejects undefined', () => stringIterator.call(undefined));
testThrows('string iterator rejects a symbol', () => stringIterator.call(Symbol('s')));

// array-likes read through getters; one that throws ends the iteration
const throwingLength = () => ({ get length() { throw new Error('length'); } });
const throwingElement = () => ({ length: 2, get 0() { throw new Error('element'); }, 1: 'b' });
testThrows('array iterator throws from a length getter', () => [...Array.prototype.values.call(throwingLength())]);
testThrows('array iterator throws from an element getter', () => [...Array.prototype.values.call(throwingElement())]);
testThrows('entries iterator throws from an element getter', () => [...Array.prototype.entries.call(throwingElement())]);
for (const helper of ['every', 'some', 'find', 'forEach']) {
  const seen = [];
  testThrows(`${helper} throws from an element getter`, () => Array.prototype.values.call(throwingElement())[helper]((x) => { seen.push(x); return true; }));
  test(`${helper} does not call back with a thrown element`, seen.length, 0);
}
test('entries iterator keeps getter results', JSON.stringify([...Array.prototype.entries.call({ length: 1, get 0() { return { fresh: 1 }; } })]), '[[0,{"fresh":1}]]');

summary();
