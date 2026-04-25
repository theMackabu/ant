import { test, summary } from './helpers.js';

console.log('Atomics Tests\n');

const sab = new SharedArrayBuffer(16);
const int32 = new Int32Array(sab);

test('SharedArrayBuffer Symbol.species', SharedArrayBuffer[Symbol.species], SharedArrayBuffer);
test('SharedArrayBuffer.prototype.byteLength exists', 'byteLength' in SharedArrayBuffer.prototype, true);
test('SharedArrayBuffer byteLength getter', sab.byteLength, 16);

Atomics.store(int32, 0, 42);
test('Atomics.store/load', Atomics.load(int32, 0), 42);

test('Atomics.add', Atomics.add(int32, 0, 10), 42);
test('after add', Atomics.load(int32, 0), 52);

test('Atomics.sub', Atomics.sub(int32, 0, 2), 52);
test('after sub', Atomics.load(int32, 0), 50);

Atomics.store(int32, 1, 0b1010);
test('Atomics.and', Atomics.and(int32, 1, 0b1100), 0b1010);
test('after and', Atomics.load(int32, 1), 0b1000);

Atomics.store(int32, 2, 0b1010);
test('Atomics.or', Atomics.or(int32, 2, 0b0101), 0b1010);
test('after or', Atomics.load(int32, 2), 0b1111);

Atomics.store(int32, 3, 0b1010);
test('Atomics.xor', Atomics.xor(int32, 3, 0b1100), 0b1010);
test('after xor', Atomics.load(int32, 3), 0b0110);

Atomics.store(int32, 0, 10);
test('Atomics.exchange', Atomics.exchange(int32, 0, 20), 10);
test('after exchange', Atomics.load(int32, 0), 20);

Atomics.store(int32, 0, 5);
test('Atomics.compareExchange match', Atomics.compareExchange(int32, 0, 5, 10), 5);
test('after compareExchange', Atomics.load(int32, 0), 10);

Atomics.store(int32, 0, 5);
test('Atomics.compareExchange no match', Atomics.compareExchange(int32, 0, 99, 10), 5);
test('after no match', Atomics.load(int32, 0), 5);

Atomics.store(int32, 0, 1);
const asyncNotEqual = Atomics.waitAsync(int32, 0, 2);
test('Atomics.waitAsync not-equal sync', asyncNotEqual.async, false);
test('Atomics.waitAsync not-equal value', asyncNotEqual.value, 'not-equal');

const asyncTimedOutSync = Atomics.waitAsync(int32, 0, 1, 0);
test('Atomics.waitAsync zero timeout sync', asyncTimedOutSync.async, false);
test('Atomics.waitAsync zero timeout value', asyncTimedOutSync.value, 'timed-out');

const timedWait = Atomics.waitAsync(int32, 0, 1, 5);
test('Atomics.waitAsync timeout is async', timedWait.async, true);
test('Atomics.waitAsync timeout value', await timedWait.value, 'timed-out');

Atomics.store(int32, 0, 3);
const notifiedWait = Atomics.waitAsync(int32, 0, 3, 1000);
test('Atomics.waitAsync notify is async', notifiedWait.async, true);
test('Atomics.notify resolves async waiter count', Atomics.notify(int32, 0, 1), 1);
test('Atomics.waitAsync notify value', await notifiedWait.value, 'ok');

Atomics.store(int32, 0, 4);
let keepChecking = true;
let waitAsyncChecks = 0;
function checkWithoutSpinning() {
  if (!keepChecking) return;
  waitAsyncChecks++;
  const result = Atomics.waitAsync(int32, 0, 4, 1000);
  if (result.async) result.value.then(checkWithoutSpinning);
  else setImmediate(checkWithoutSpinning);
}
checkWithoutSpinning();
await new Promise(resolve => setImmediate(resolve));
test('pending waitAsync yields to immediates', waitAsyncChecks, 1);
keepChecking = false;
Atomics.notify(int32, 0, 1);

summary();
