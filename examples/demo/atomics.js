console.log('1. Creating SharedArrayBuffer');
const sharedBuffer = new SharedArrayBuffer(256);
console.log('   SharedArrayBuffer created with size:', sharedBuffer.byteLength, 'bytes\n');

console.log('2. Creating TypedArrays on SharedArrayBuffer');
const int32View = new Int32Array(sharedBuffer);
const uint8View = new Uint8Array(sharedBuffer);
console.log('   Int32Array length:', int32View.length);
console.log('   Uint8Array length:', uint8View.length, '\n');

console.log('3. Basic atomic store and load');
Atomics.store(int32View, 0, 42);
const value = Atomics.load(int32View, 0);
console.log('   Stored 42, loaded:', value, '\n');

console.log('4. Atomic add operation');
Atomics.store(int32View, 1, 10);
const oldValue = Atomics.add(int32View, 1, 5);
const newValue = Atomics.load(int32View, 1);
console.log('   Old value:', oldValue);
console.log('   Added 5, new value:', newValue, '\n');

console.log('5. Compare and exchange');
Atomics.store(int32View, 2, 100);
console.log('   Initial value:', Atomics.load(int32View, 2));

const result1 = Atomics.compareExchange(int32View, 2, 50, 200);
console.log('   Expected 50, got:', result1, '(no change)');
console.log('   Current value:', Atomics.load(int32View, 2));

const result2 = Atomics.compareExchange(int32View, 2, 100, 200);
console.log('   Expected 100, got:', result2, '(changed!)');
console.log('   Current value:', Atomics.load(int32View, 2), '\n');

console.log('6. Bitwise operations');
Atomics.store(uint8View, 0, 0b11110000);
console.log('   Initial: 0b' + Atomics.load(uint8View, 0).toString(2).padStart(8, '0'));

Atomics.and(uint8View, 0, 0b00111100);
console.log('   After AND with 0b00111100: 0b' + Atomics.load(uint8View, 0).toString(2).padStart(8, '0'));

Atomics.or(uint8View, 0, 0b00000011);
console.log('   After OR with 0b00000011: 0b' + Atomics.load(uint8View, 0).toString(2).padStart(8, '0'));

Atomics.xor(uint8View, 0, 0b11111111);
console.log('   After XOR with 0b11111111: 0b' + Atomics.load(uint8View, 0).toString(2).padStart(8, '0'), '\n');

console.log('7. Lock-free operations check');
console.log('   1 byte:', Atomics.isLockFree(1) ? 'Lock-free ✓' : 'Uses locks');
console.log('   2 bytes:', Atomics.isLockFree(2) ? 'Lock-free ✓' : 'Uses locks');
console.log('   4 bytes:', Atomics.isLockFree(4) ? 'Lock-free ✓' : 'Uses locks');
console.log('   8 bytes:', Atomics.isLockFree(8) ? 'Lock-free ✓' : 'Uses locks', '\n');

console.log('8. Atomic counter example');
Atomics.store(int32View, 10, 0);
console.log('   Counter initialized to:', Atomics.load(int32View, 10));

for (let i = 0; i < 10; i++) {
  Atomics.add(int32View, 10, 1);
}
console.log('   After 10 atomic increments:', Atomics.load(int32View, 10), '\n');

console.log('9. Spin-lock pattern example');
const LOCK_INDEX = 20;
const UNLOCKED = 0;
const LOCKED = 1;

Atomics.store(int32View, LOCK_INDEX, UNLOCKED);
console.log('   Lock initialized');

function tryAcquireLock() {
  return Atomics.compareExchange(int32View, LOCK_INDEX, UNLOCKED, LOCKED) === UNLOCKED;
}

function releaseLock() {
  Atomics.store(int32View, LOCK_INDEX, UNLOCKED);
}

if (tryAcquireLock()) {
  console.log('   Lock acquired ✓');
  console.log('   Performing critical operation...');
  releaseLock();
  console.log('   Lock released ✓');
} else {
  console.log('   Failed to acquire lock');
}

console.log('\n=== Example completed successfully ===');
