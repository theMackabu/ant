import { test, testDeep, summary } from './helpers.js';

console.log('Top-Level Await Tests\n');

// basic TLA with promise
const basic = await Promise.resolve(42);
test('basic TLA', basic, 42);

// TLA with async function call
async function getValue() { return 'hello'; }
const fromAsync = await getValue();
test('TLA await async function', fromAsync, 'hello');

// TLA sequential awaits
const a = await Promise.resolve(1);
const b = await Promise.resolve(2);
const c = await Promise.resolve(3);
test('TLA sequential awaits', a + b + c, 6);

// TLA with non-promise (should pass through)
const plain = await 99;
test('TLA await non-promise', plain, 99);

// TLA with chained promise
const chained = await Promise.resolve(1).then(v => v + 1).then(v => v + 1);
test('TLA chained promise', chained, 3);

// TLA preserves execution order
const order = [];
order.push(1);
await Promise.resolve();
order.push(2);
await Promise.resolve();
order.push(3);
testDeep('TLA preserves order', order, [1, 2, 3]);

// TLA with Promise.all
const [x, y, z] = await Promise.all([
  Promise.resolve('a'),
  Promise.resolve('b'),
  Promise.resolve('c'),
]);
test('TLA Promise.all destructuring', x + y + z, 'abc');

// TLA error handling with try/catch
let caught = null;
try {
  await Promise.reject('tla error');
} catch (e) {
  caught = e;
}
test('TLA try/catch rejection', caught, 'tla error');

// TLA with setTimeout promise
const delayed = await new Promise(resolve => setTimeout(() => resolve('delayed'), 10));
test('TLA with setTimeout', delayed, 'delayed');

// TLA in conditional
const flag = true;
const conditional = flag ? await Promise.resolve('yes') : 'no';
test('TLA in conditional', conditional, 'yes');

summary();
