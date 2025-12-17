import { test, testDeep, summary } from './helpers.js';

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

const map = new Map([['a', 1], ['b', 2]]);
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

summary();
