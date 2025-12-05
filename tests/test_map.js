// Test Map functionality
const map = new Map();

// Test basic set/get
map.set('key1', 'value1');
map.set('key2', 42);
map.set('key3', true);

console.log('Map size:', map.size()); // Should be 3
console.log('Get key1:', map.get('key1')); // Should be 'value1'
console.log('Get key2:', map.get('key2')); // Should be 42
console.log('Get key3:', map.get('key3')); // Should be true

// Test has()
console.log('Has key1:', map.has('key1')); // Should be true
console.log('Has missing:', map.has('missing')); // Should be false

// Test delete()
console.log('Delete key2:', map.delete('key2')); // Should be true
console.log('Has key2 after delete:', map.has('key2')); // Should be false
console.log('Map size after delete:', map.size()); // Should be 2

// Test overwrite
map.set('key1', 'newvalue1');
console.log('Get key1 after overwrite:', map.get('key1')); // Should be 'newvalue1'

// Test clear()
map.clear();
console.log('Map size after clear:', map.size()); // Should be 0
console.log('Has key1 after clear:', map.has('key1')); // Should be false

// Test with different key types
map.set(123, 'number key');
map.set(true, 'boolean key');
map.set(null, 'null key');

console.log('Get number key:', map.get(123)); // Should be 'number key'
console.log('Get boolean key:', map.get(true)); // Should be 'boolean key'
console.log('Get null key:', map.get(null)); // Should be 'null key'
console.log('Final map size:', map.size()); // Should be 3

console.log('Map tests completed!');