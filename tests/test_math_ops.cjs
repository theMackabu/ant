// Test exponentiation operator
console.log('Exponentiation tests:');
console.log('2 ** 3 =', 2 ** 3);  // Should be 8
console.log('5 ** 2 =', 5 ** 2);  // Should be 25
console.log('10 ** 0 =', 10 ** 0);  // Should be 1
console.log('2 ** 10 =', 2 ** 10);  // Should be 1024
console.log('3 ** 3 =', 3 ** 3);  // Should be 27

// Test bit shift operators
console.log('\nBit shift tests:');
console.log('8 << 2 =', 8 << 2);  // Should be 32 (8 * 4)
console.log('32 >> 2 =', 32 >> 2);  // Should be 8 (32 / 4)
console.log('1 << 5 =', 1 << 5);  // Should be 32
console.log('64 >> 3 =', 64 >> 3);  // Should be 8

// Test bitwise operators
console.log('\nBitwise tests:');
console.log('12 & 10 =', 12 & 10);  // Should be 8 (binary: 1100 & 1010 = 1000)
console.log('12 | 10 =', 12 | 10);  // Should be 14 (binary: 1100 | 1010 = 1110)
console.log('12 ^ 10 =', 12 ^ 10);  // Should be 6 (binary: 1100 ^ 1010 = 0110)
console.log('~5 =', ~5);  // Should be -6

// Test combined operations
console.log('\nCombined operations:');
console.log('2 ** 3 + 4 =', 2 ** 3 + 4);  // Should be 12
console.log('(2 + 3) ** 2 =', (2 + 3) ** 2);  // Should be 25
console.log('2 << 3 >> 1 =', 2 << 3 >> 1);  // Should be 8

// Test with negative numbers
console.log('\nNegative number tests:');
console.log('(-2) ** 3 =', (-2) ** 3);  // Should be -8
console.log('-8 >> 1 =', -8 >> 1);  // Should be -4
