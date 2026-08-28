import assert from 'node:assert';

const order = [];
const controller = new AbortController();

controller.signal.onabort = () => {
  order.push('onabort');
  Promise.resolve().then(() => order.push('microtask'));
};
controller.signal.addEventListener('abort', () => order.push('listener-1'));
controller.signal.addEventListener('abort', () => order.push('listener-2'));

controller.abort();
order.push('after-abort');

assert.deepStrictEqual(order, [
  'onabort',
  'listener-1',
  'listener-2',
  'after-abort',
]);

await Promise.resolve();

assert.deepStrictEqual(order, [
  'onabort',
  'listener-1',
  'listener-2',
  'after-abort',
  'microtask',
]);

console.log('abort listeners finish before promise jobs run');
