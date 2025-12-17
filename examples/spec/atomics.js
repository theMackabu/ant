import { test, summary } from './helpers.js';

console.log('Atomics Tests\n');

const sab = new SharedArrayBuffer(16);
const int32 = new Int32Array(sab);

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

summary();
