// Test WeakMap functionality
console.log('=== WeakMap Tests ===');

const wm = new WeakMap();

// Test with object keys
const key1 = { id: 1 };
const key2 = { id: 2 };
const key3 = { id: 3 };

// Test set and get
wm.set(key1, 'value1');
wm.set(key2, { data: 42 });
wm.set(key3, true);

console.log('Get key1:', wm.get(key1)); // Should be 'value1'
console.log('Get key2:', wm.get(key2)); // Should be { data: 42 }
console.log('Get key3:', wm.get(key3)); // Should be true

// Test has()
console.log('Has key1:', wm.has(key1)); // Should be true
console.log('Has key2:', wm.has(key2)); // Should be true
const key4 = { id: 4 };
console.log('Has key4 (not added):', wm.has(key4)); // Should be false

// Test delete()
console.log('Delete key2:', wm.delete(key2)); // Should be true
console.log('Has key2 after delete:', wm.has(key2)); // Should be false
console.log('Get key2 after delete:', wm.get(key2)); // Should be undefined

// Test that key1 and key3 are still there
console.log('Has key1 after delete key2:', wm.has(key1)); // Should be true
console.log('Has key3 after delete key2:', wm.has(key3)); // Should be true

// Test overwrite
wm.set(key1, 'newvalue1');
console.log('Get key1 after overwrite:', wm.get(key1)); // Should be 'newvalue1'

// Test that primitive keys throw errors
try {
  wm.set('string', 'value');
  console.log('ERROR: Should have thrown for string key');
} catch (e) {
  console.log('Correctly threw error for string key:', e.message);
}

try {
  wm.set(123, 'value');
  console.log('ERROR: Should have thrown for number key');
} catch (e) {
  console.log('Correctly threw error for number key:', e.message);
}

try {
  wm.set(null, 'value');
  console.log('ERROR: Should have thrown for null key');
} catch (e) {
  console.log('Correctly threw error for null key:', e.message);
}

// Test get/has/delete with non-object keys (should return undefined/false)
console.log('Get with string key:', wm.get('string')); // Should be undefined
console.log('Has with number key:', wm.has(123)); // Should be false
console.log('Delete with boolean key:', wm.delete(true)); // Should be false

// Test multiple WeakMaps
const wm2 = new WeakMap();
wm2.set(key1, 'different value');
console.log('Get key1 from wm:', wm.get(key1)); // Should be 'newvalue1'
console.log('Get key1 from wm2:', wm2.get(key1)); // Should be 'different value'

console.log('WeakMap tests completed!');
