// Test WeakSet functionality
console.log('=== WeakSet Tests ===');

const ws = new WeakSet();

// Test with object values
const obj1 = { id: 1 };
const obj2 = { id: 2 };
const obj3 = { id: 3 };

// Test add and has
ws.add(obj1);
ws.add(obj2);
ws.add(obj3);

console.log('Has obj1:', ws.has(obj1)); // Should be true
console.log('Has obj2:', ws.has(obj2)); // Should be true
console.log('Has obj3:', ws.has(obj3)); // Should be true

const obj4 = { id: 4 };
console.log('Has obj4 (not added):', ws.has(obj4)); // Should be false

// Test delete()
console.log('Delete obj2:', ws.delete(obj2)); // Should be true
console.log('Has obj2 after delete:', ws.has(obj2)); // Should be false

// Test that obj1 and obj3 are still there
console.log('Has obj1 after delete obj2:', ws.has(obj1)); // Should be true
console.log('Has obj3 after delete obj2:', ws.has(obj3)); // Should be true

// Test adding the same object twice (should not throw)
ws.add(obj1);
console.log('Has obj1 after re-adding:', ws.has(obj1)); // Should still be true

// Test that primitive values throw errors
try {
  ws.add('string');
  console.log('ERROR: Should have thrown for string value');
} catch (e) {
  console.log('Correctly threw error for string value:', e.message);
}

try {
  ws.add(123);
  console.log('ERROR: Should have thrown for number value');
} catch (e) {
  console.log('Correctly threw error for number value:', e.message);
}

try {
  ws.add(null);
  console.log('ERROR: Should have thrown for null value');
} catch (e) {
  console.log('Correctly threw error for null value:', e.message);
}

try {
  ws.add(undefined);
  console.log('ERROR: Should have thrown for undefined value');
} catch (e) {
  console.log('Correctly threw error for undefined value:', e.message);
}

// Test has/delete with non-object values (should return false)
console.log('Has with string:', ws.has('string')); // Should be false
console.log('Has with number:', ws.has(123)); // Should be false
console.log('Delete with boolean:', ws.delete(true)); // Should be false

// Test multiple WeakSets with the same object
const ws2 = new WeakSet();
ws2.add(obj1);
console.log('Has obj1 in ws:', ws.has(obj1)); // Should be true
console.log('Has obj1 in ws2:', ws2.has(obj1)); // Should be true

// Delete from ws should not affect ws2
ws.delete(obj1);
console.log('Has obj1 in ws after delete:', ws.has(obj1)); // Should be false
console.log('Has obj1 in ws2 after delete from ws:', ws2.has(obj1)); // Should be true

// Test chaining
ws.add(obj1).add(obj2);
console.log('Has obj1 after chaining:', ws.has(obj1)); // Should be true
console.log('Has obj2 after chaining:', ws.has(obj2)); // Should be true

console.log('WeakSet tests completed!');
