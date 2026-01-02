console.log('=== Simple GC Test ===\n');

// Test 1: Basic objects
console.log('Test 1: Objects');
let obj1 = { name: 'test', value: 42 };
console.log('Before GC:', obj1.name);
Ant.gc();
console.log('After GC:', obj1.name);

// Test 2: Map
console.log('\nTest 2: Map');
let map = new Map();
map.set('key1', 'value1');
console.log('Before GC:', map.get('key1'));
Ant.gc();
console.log('After GC:', map.get('key1'));

console.log('\n=== Done ===');
