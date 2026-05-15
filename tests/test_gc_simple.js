const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

console.log('=== Simple GC Test ===\n');

// Test 1: Basic objects
console.log('Test 1: Objects');
let obj1 = { name: 'test', value: 42 };
console.log('After GC:', obj1.name);

// Test 2: Map
console.log('\nTest 2: Map');
let map = new Map();
map.set('key1', 'value1');
console.log('After GC:', map.get('key1'));

console.log('\n=== Done ===');
__gcPerfLog();
