let d = new Date();

// Test 1: Default behavior
console.log(d[Symbol.toPrimitive]('default')); // Should call toString

// Test 2: String hint
console.log(d[Symbol.toPrimitive]('string')); // Should call toString

// Test 3: Number hint
console.log(d[Symbol.toPrimitive]('number')); // Should call valueOf (timestamp)

// Test 4: Coercion tests
console.log(d + ''); // Uses default hint -> string
console.log(+d); // Uses number hint -> number
console.log(d - 0); // Uses number hint -> number

// Test 5: Verify order - SAVE ORIGINAL METHODS FIRST
const originalValueOf = Date.prototype.valueOf;
const originalToString = Date.prototype.toString;

Date.prototype.valueOf = function () {
  console.log('valueOf called');
  return originalValueOf.call(this); // Call the SAVED original
};
Date.prototype.toString = function () {
  console.log('toString called');
  return originalToString.call(this); // Call the SAVED original
};

let d2 = new Date();
console.log('\nTesting string hint:');
d2[Symbol.toPrimitive]('string'); // Should log: toString only (returns primitive)

console.log('\nTesting number hint:');
d2[Symbol.toPrimitive]('number'); // Should log: valueOf only (returns primitive)

console.log('\nTesting default hint:');
d2[Symbol.toPrimitive]('default'); // Should log: toString only (returns primitive)

// Restore original methods
Date.prototype.valueOf = originalValueOf;
Date.prototype.toString = originalToString;
