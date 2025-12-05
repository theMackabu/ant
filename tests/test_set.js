// Test Set functionality
const set = new Set();

// Test basic add/has
set.add('value1');
set.add('value2');
set.add(42);
set.add(true);

console.log('Set size:', set.size()); // Should be 4
console.log('Has value1:', set.has('value1')); // Should be true
console.log('Has value2:', set.has('value2')); // Should be true
console.log('Has 42:', set.has(42)); // Should be true
console.log('Has true:', set.has(true)); // Should be true
console.log('Has missing:', set.has('missing')); // Should be false

// Test delete()
console.log('Delete value2:', set.delete('value2')); // Should be true
console.log('Has value2 after delete:', set.has('value2')); // Should be false
console.log('Set size after delete:', set.size()); // Should be 3

// Test adding duplicates (should not increase size)
set.add('value1'); // Adding duplicate
console.log('Set size after duplicate add:', set.size()); // Should still be 3

// Test with different value types
set.add(123);
set.add(null);
set.add({key: 'object'});

console.log('Has 123:', set.has(123)); // Should be true
console.log('Has null:', set.has(null)); // Should be true
console.log('Has object:', set.has({key: 'object'})); // Should be true
console.log('Final set size:', set.size()); // Should be 7

// Test clear()
set.clear();
console.log('Set size after clear:', set.size()); // Should be 0
console.log('Has value1 after clear:', set.has('value1')); // Should be false

// Test chaining
set.add('a').add('b').add('c');
console.log('Set size after chaining:', set.size()); // Should be 3
console.log('Has a:', set.has('a')); // Should be true
console.log('Has b:', set.has('b')); // Should be true
console.log('Has c:', set.has('c')); // Should be true

console.log('Set tests completed!');