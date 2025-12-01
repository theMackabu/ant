// Test Date builtin functionality

// Test 1: Date.now() returns a number
console.log('Test 1: Date.now() returns a number');
const now1 = Date.now();
console.log('Date.now() =', now1);
console.log('typeof Date.now() =', typeof now1);

// Test 2: Date.now() returns milliseconds since epoch
console.log('\nTest 2: Date.now() returns reasonable timestamp');
const now2 = Date.now();
// Should be a large number (milliseconds since 1970)
console.log('Date.now() > 1000000000000 ?', now2 > 1000000000000);

// Test 3: new Date() creates an object
console.log('\nTest 3: new Date() creates an object');
const d1 = new Date();
console.log('typeof new Date() =', typeof d1);
console.log('new Date() =', d1);

// Test 4: new Date(timestamp) works
console.log('\nTest 4: new Date(timestamp)');
const d2 = new Date(1234567890000);
console.log('new Date(1234567890000) =', d2);

// Test 5: Date.now() is monotonic (or at least consistent)
console.log('\nTest 5: Date.now() consistency');
const t1 = Date.now();
const t2 = Date.now();
console.log('t1 =', t1);
console.log('t2 =', t2);
console.log('t2 >= t1 ?', t2 >= t1);

// Test 6: Multiple calls to Date.now() return numbers
console.log('\nTest 6: Multiple Date.now() calls');
const times = [];
for (let i = 0; i < 3; i++) {
  times.push(Date.now());
}
console.log('Times:', times);
console.log('All are numbers?', times.every(t => typeof t === 'number'));

// Test 7: Date constructor with no args vs Date.now()
console.log('\nTest 7: Compare new Date() and Date.now()');
const dateObj = new Date();
const nowTime = Date.now();
console.log('new Date() created:', dateObj);
console.log('Date.now() =', nowTime);

console.log('\nAll Date tests completed!');
