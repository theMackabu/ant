console.log('=== Atomics API Tests ===\n');

// Test 1: SharedArrayBuffer creation
console.log('Test 1: SharedArrayBuffer creation');
const sab = new SharedArrayBuffer(1024);
console.log('SharedArrayBuffer byteLength:', sab.byteLength);
console.log('SharedArrayBuffer created:', sab.byteLength === 1024 ? 'PASS' : 'FAIL');

// Test 2: Int32Array on SharedArrayBuffer
console.log('\nTest 2: Int32Array on SharedArrayBuffer');
const ta = new Int32Array(sab);
console.log('Int32Array length:', ta.length);
console.log('Int32Array byteLength:', ta.byteLength);

// Test 3: Atomics.store and Atomics.load
console.log('\nTest 3: Atomics.store and Atomics.load');
ta[0] = 0;
console.log('Initial value:', ta[0]);
Atomics.store(ta, 0, 12);
const loaded = Atomics.load(ta, 0);
console.log('After Atomics.store(ta, 0, 12):', loaded);
console.log('Atomics.store/load test:', loaded === 12 ? 'PASS' : 'FAIL');

// Test 4: Atomics.add
console.log('\nTest 4: Atomics.add');
Atomics.store(ta, 0, 5);
const oldAdd = Atomics.add(ta, 0, 12);
const newAdd = Atomics.load(ta, 0);
console.log('Old value:', oldAdd);
console.log('New value after add(12):', newAdd);
console.log('Atomics.add test:', oldAdd === 5 && newAdd === 17 ? 'PASS' : 'FAIL');

// Test 5: Atomics.sub
console.log('\nTest 5: Atomics.sub');
Atomics.store(ta, 0, 12);
const oldSub = Atomics.sub(ta, 0, 2);
const newSub = Atomics.load(ta, 0);
console.log('Old value:', oldSub);
console.log('New value after sub(2):', newSub);
console.log('Atomics.sub test:', oldSub === 12 && newSub === 10 ? 'PASS' : 'FAIL');

// Test 6: Atomics.and
console.log('\nTest 6: Atomics.and');
Atomics.store(ta, 0, 17);
const oldAnd = Atomics.and(ta, 0, 1);
const newAnd = Atomics.load(ta, 0);
console.log('Old value (17):', oldAnd);
console.log('New value after and(1):', newAnd);
console.log('Atomics.and test:', oldAnd === 17 && newAnd === 1 ? 'PASS' : 'FAIL');

// Test 7: Atomics.or
console.log('\nTest 7: Atomics.or');
Atomics.store(ta, 0, 12);
const oldOr = Atomics.or(ta, 0, 1);
const newOr = Atomics.load(ta, 0);
console.log('Old value (12):', oldOr);
console.log('New value after or(1):', newOr);
console.log('Atomics.or test:', oldOr === 12 && newOr === 13 ? 'PASS' : 'FAIL');

// Test 8: Atomics.xor
console.log('\nTest 8: Atomics.xor');
Atomics.store(ta, 0, 10);
const oldXor = Atomics.xor(ta, 0, 1);
const newXor = Atomics.load(ta, 0);
console.log('Old value (10):', oldXor);
console.log('New value after xor(1):', newXor);
console.log('Atomics.xor test:', oldXor === 10 && newXor === 11 ? 'PASS' : 'FAIL');

// Test 9: Atomics.exchange
console.log('\nTest 9: Atomics.exchange');
Atomics.store(ta, 0, 1);
const oldExchange = Atomics.exchange(ta, 0, 12);
const newExchange = Atomics.load(ta, 0);
console.log('Old value:', oldExchange);
console.log('New value after exchange(12):', newExchange);
console.log('Atomics.exchange test:', oldExchange === 1 && newExchange === 12 ? 'PASS' : 'FAIL');

// Test 10: Atomics.compareExchange (match)
console.log('\nTest 10: Atomics.compareExchange (match)');
Atomics.store(ta, 0, 5);
const resultMatch = Atomics.compareExchange(ta, 0, 5, 12);
const valueMatch = Atomics.load(ta, 0);
console.log('Expected 5, got:', resultMatch);
console.log('New value:', valueMatch);
console.log('Atomics.compareExchange (match) test:', resultMatch === 5 && valueMatch === 12 ? 'PASS' : 'FAIL');

// Test 11: Atomics.compareExchange (no match)
console.log('\nTest 11: Atomics.compareExchange (no match)');
Atomics.store(ta, 0, 1);
const resultNoMatch = Atomics.compareExchange(ta, 0, 5, 12);
const valueNoMatch = Atomics.load(ta, 0);
console.log('Expected 5, got:', resultNoMatch);
console.log('Value unchanged:', valueNoMatch);
console.log('Atomics.compareExchange (no match) test:', resultNoMatch === 1 && valueNoMatch === 1 ? 'PASS' : 'FAIL');

// Test 12: Atomics.isLockFree
console.log('\nTest 12: Atomics.isLockFree');
console.log('isLockFree(1):', Atomics.isLockFree(1));
console.log('isLockFree(2):', Atomics.isLockFree(2));
console.log('isLockFree(3):', Atomics.isLockFree(3));
console.log('isLockFree(4):', Atomics.isLockFree(4));
console.log('isLockFree(8):', Atomics.isLockFree(8));
console.log('Atomics.isLockFree test:', Atomics.isLockFree(4) === true ? 'PASS' : 'FAIL');

// Test 13: Atomics.notify (without waiters)
console.log('\nTest 13: Atomics.notify (without waiters)');
const int32 = new Int32Array(sab);
Atomics.store(int32, 0, 0);
const notified = Atomics.notify(int32, 0, 1);
console.log('Agents notified:', notified);
console.log('Atomics.notify test:', notified === 0 ? 'PASS' : 'FAIL');

// Test 14: Comprehensive example from MDN
console.log('\nTest 14: Comprehensive example');
const sab2 = new SharedArrayBuffer(1024);
const ta2 = new Uint8Array(sab2);

ta2[0] = 0;
console.log('ta2[0]:', ta2[0]); // 0
ta2[0] = 5;
console.log('ta2[0] = 5:', ta2[0]); // 5

Atomics.add(ta2, 0, 12);
console.log('After Atomics.add(ta2, 0, 12):', Atomics.load(ta2, 0)); // 17

Atomics.and(ta2, 0, 1);
console.log('After Atomics.and(ta2, 0, 1):', Atomics.load(ta2, 0)); // 1

Atomics.compareExchange(ta2, 0, 5, 12);
console.log('After Atomics.compareExchange(ta2, 0, 5, 12):', Atomics.load(ta2, 0)); // 1 (no change)

Atomics.exchange(ta2, 0, 12);
console.log('After Atomics.exchange(ta2, 0, 12):', Atomics.load(ta2, 0)); // 12

Atomics.or(ta2, 0, 1);
console.log('After Atomics.or(ta2, 0, 1):', Atomics.load(ta2, 0)); // 13

Atomics.store(ta2, 0, 12);
console.log('After Atomics.store(ta2, 0, 12):', Atomics.load(ta2, 0)); // 12

Atomics.sub(ta2, 0, 2);
console.log('After Atomics.sub(ta2, 0, 2):', Atomics.load(ta2, 0)); // 10

Atomics.xor(ta2, 0, 1);
console.log('After Atomics.xor(ta2, 0, 1):', Atomics.load(ta2, 0)); // 11

console.log('\n=== All Atomics tests completed ===');
