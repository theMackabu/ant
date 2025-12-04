// Test NaN and Infinity values
console.log('=== NaN Tests ===');

// Basic NaN value
console.log('NaN:', NaN);
console.log('typeof NaN:', typeof NaN);

// Type coercion resulting in NaN
console.log('"wat" - 1:', "wat" - 1);
console.log('"hello" * 2:', "hello" * 2);
console.log('undefined + 5:', undefined + 5);
console.log('10 / "abc":', 10 / "abc");
console.log('0/0:', 0/0);

// Valid string to number coercion
console.log('"5" - 2:', "5" - 2);
console.log('"10" * 2:', "10" * 2);

// Boolean and null coercion
console.log('true + 1:', true + 1);
console.log('false + 1:', false + 1);
console.log('null + 5:', null + 5);

// NaN properties
console.log('NaN === NaN:', NaN === NaN);  // Should be false
console.log('NaN !== NaN:', NaN !== NaN);  // Should be true

// Number.isNaN tests
console.log('\n=== Number.isNaN Tests ===');
console.log('Number.isNaN(NaN):', Number.isNaN(NaN));
console.log('Number.isNaN("wat" - 1):', Number.isNaN("wat" - 1));
console.log('Number.isNaN(123):', Number.isNaN(123));
console.log('Number.isNaN("hello"):', Number.isNaN("hello"));  // Should be false (not a number type)
console.log('Number.isNaN(undefined):', Number.isNaN(undefined));  // Should be false
console.log('Number.isNaN(null):', Number.isNaN(null));  // Should be false

// parseInt/parseFloat with invalid input
console.log('\n=== parseInt/parseFloat NaN ===');
console.log('parseInt("notanumber"):', parseInt("notanumber"));
console.log('parseInt("123abc"):', parseInt("123abc"));
console.log('parseFloat("xyz"):', parseFloat("xyz"));

// Infinity tests
console.log('\n=== Infinity Tests ===');
console.log('Infinity:', Infinity);
console.log('typeof Infinity:', typeof Infinity);
console.log('1/0:', 1/0);
console.log('-1/0:', -1/0);

// Number.isFinite tests
console.log('\n=== Number.isFinite Tests ===');
console.log('Number.isFinite(123):', Number.isFinite(123));
console.log('Number.isFinite(1/0):', Number.isFinite(1/0));
console.log('Number.isFinite(-1/0):', Number.isFinite(-1/0));
console.log('Number.isFinite(NaN):', Number.isFinite(NaN));
console.log('Number.isFinite(Infinity):', Number.isFinite(Infinity));
console.log('Number.isFinite("123"):', Number.isFinite("123"));  // Should be false (not a number type)

// Array.join with NaN
console.log('\n=== Array.join with NaN ===');
console.log('[1,2,3].join(NaN):', [1,2,3].join(NaN));
console.log('[1,2,3].join(0/0):', [1,2,3].join(0/0));
console.log('Array(16).join("wat" - 1) + " Batman!":', Array(16).join("wat" - 1) + " Batman!");

// String operations with NaN
console.log('\n=== String operations ===');
console.log('"Result: " + NaN:', "Result: " + NaN);
console.log('"Value: " + (0/0):', "Value: " + (0/0));

// Arithmetic with Infinity
console.log('\n=== Arithmetic with Infinity ===');
console.log('Infinity + 1:', Infinity + 1);
console.log('Infinity - 1:', Infinity - 1);
console.log('Infinity * 2:', Infinity * 2);
console.log('Infinity / 2:', Infinity / 2);
console.log('Infinity - Infinity:', Infinity - Infinity);
console.log('Infinity * 0:', Infinity * 0);

console.log('\n=== All tests completed ===');
