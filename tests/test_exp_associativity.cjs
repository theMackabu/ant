// Test right-associativity of exponentiation
// 2 ** 3 ** 2 should be 2 ** (3 ** 2) = 2 ** 9 = 512
// NOT (2 ** 3) ** 2 = 8 ** 2 = 64
console.log('2 ** 3 ** 2 =', 2 ** 3 ** 2);
console.log('Expected: 512 (right-associative)');
console.log('(2 ** 3) ** 2 =', (2 ** 3) ** 2);
console.log('Expected: 64 (left-associative, for comparison)');
