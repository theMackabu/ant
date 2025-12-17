import { test, testDeep, summary } from './helpers.js';

console.log('Destructuring Tests\n');

const [a, b, c] = [1, 2, 3];
test('array destructure a', a, 1);
test('array destructure b', b, 2);
test('array destructure c', c, 3);

const [first, , third] = [1, 2, 3];
test('skip element first', first, 1);
test('skip element third', third, 3);

const [x, y, z = 10] = [1, 2];
test('array default x', x, 1);
test('array default z', z, 10);

const [head, ...tail] = [1, 2, 3, 4];
test('array rest head', head, 1);
testDeep('array rest tail', tail, [2, 3, 4]);

const { name, age } = { name: 'Alice', age: 30 };
test('object destructure name', name, 'Alice');
test('object destructure age', age, 30);

const { foo: renamed } = { foo: 'bar' };
test('object rename', renamed, 'bar');

const { val = 'default' } = {};
test('object default', val, 'default');

const { p, ...rest } = { p: 1, q: 2, r: 3 };
test('object rest p', p, 1);
test('object rest has q', rest.q, 2);
test('object rest has r', rest.r, 3);

const nested = { outer: { inner: 42 } };
const { outer: { inner } } = nested;
test('nested destructure', inner, 42);

function destruct({ a, b }) {
  return a + b;
}
test('param destructure', destruct({ a: 3, b: 4 }), 7);

function arrayParam([x, y]) {
  return x * y;
}
test('array param destructure', arrayParam([3, 4]), 12);

const swap = ([a, b]) => [b, a];
testDeep('swap via destructure', swap([1, 2]), [2, 1]);

summary();
