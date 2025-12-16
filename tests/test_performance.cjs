// Test performance API
console.log('Testing performance API...');

// Test that performance object exists
console.log('performance exists:', typeof performance === 'object');

// Test performance.now()
console.log('performance.now exists:', typeof performance.now === 'function');

const t0 = performance.now();
console.log('performance.now() returns number:', typeof t0 === 'number');
console.log('performance.now() >= 0:', t0 >= 0);

// Test performance.timeOrigin
console.log('performance.timeOrigin exists:', typeof performance.timeOrigin === 'number');
console.log('performance.timeOrigin > 0:', performance.timeOrigin > 0);

// Test that now() increases over time
const t1 = performance.now();
console.log('performance.now() increases:', t1 >= t0);

// Test timing a simple operation
const start = performance.now();
let sum = 0;
for (let i = 0; i < 10000; i++) {
  sum += i;
}
const end = performance.now();
const elapsed = end - start;

console.log('Timing test - elapsed ms:', elapsed);
console.log('Timing test - elapsed >= 0:', elapsed >= 0);

// Test that timeOrigin + now() gives current time (approximately)
const currentTime = performance.timeOrigin + performance.now();
console.log('timeOrigin + now() is reasonable timestamp:', currentTime > 1700000000000);

console.log('All performance tests completed!');
