console.log('=== Numeric Separator Tests ===\n');

// Test 1: Decimal separators
console.log('Test 1: Decimal separators');
console.log('1_000:', 1_000);
console.log('1_000_000:', 1_000_000);
console.log('86_400_000:', 86_400_000);
console.log('1_000 === 1000:', 1_000 === 1000 ? 'PASS' : 'FAIL');
console.log('1_000_000 === 1000000:', 1_000_000 === 1000000 ? 'PASS' : 'FAIL');
console.log('86_400_000 === 86400000:', 86_400_000 === 86400000 ? 'PASS' : 'FAIL');

// Test 2: Hexadecimal separators
console.log('\nTest 2: Hexadecimal separators');
console.log('0xFF_FF:', 0xFF_FF);
console.log('0xDE_AD_BE_EF:', 0xDE_AD_BE_EF);
console.log('0xFF_AA_BB:', 0xFF_AA_BB);
console.log('0xFF_FF === 65535:', 0xFF_FF === 65535 ? 'PASS' : 'FAIL');
console.log('0xDE_AD_BE_EF === 3735928559:', 0xDE_AD_BE_EF === 3735928559 ? 'PASS' : 'FAIL');

// Test 3: Binary separators
console.log('\nTest 3: Binary separators');
console.log('0b1010_1010:', 0b1010_1010);
console.log('0b1111_0000_1111_0000:', 0b1111_0000_1111_0000);
console.log('0b1010_1010 === 170:', 0b1010_1010 === 170 ? 'PASS' : 'FAIL');
console.log('0b1111_0000_1111_0000 === 61680:', 0b1111_0000_1111_0000 === 61680 ? 'PASS' : 'FAIL');

// Test 4: Octal separators
console.log('\nTest 4: Octal separators');
console.log('0o77_00:', 0o77_00);
console.log('0o123_456:', 0o123_456);
console.log('0o77_00 === 4032:', 0o77_00 === 4032 ? 'PASS' : 'FAIL');
console.log('0o123_456 === 42798:', 0o123_456 === 42798 ? 'PASS' : 'FAIL');

// Test 5: Floating point separators
console.log('\nTest 5: Floating point separators');
console.log('1_000.5:', 1_000.5);
console.log('1_000.123_456:', 1_000.123_456);
console.log('1_000.5 === 1000.5:', 1_000.5 === 1000.5 ? 'PASS' : 'FAIL');
console.log('1_000.123_456 === 1000.123456:', 1_000.123_456 === 1000.123456 ? 'PASS' : 'FAIL');

// Test 6: Mixed expressions
console.log('\nTest 6: Mixed expressions');
const sum = 1_000 + 2_000 + 3_000;
console.log('1_000 + 2_000 + 3_000 =', sum);
console.log('Sum === 6000:', sum === 6000 ? 'PASS' : 'FAIL');

const product = 10_00 * 10_0;
console.log('10_00 * 10_0 =', product);
console.log('Product === 100000:', product === 100000 ? 'PASS' : 'FAIL');

// Test 7: Comparison with regular literals
console.log('\nTest 7: Comparison with regular literals');
console.log('1_2_3_4_5 === 12345:', 1_2_3_4_5 === 12345 ? 'PASS' : 'FAIL');
console.log('0xA_B_C === 0xABC:', 0xA_B_C === 0xABC ? 'PASS' : 'FAIL');
console.log('0b1_0_1_0 === 0b1010:', 0b1_0_1_0 === 0b1010 ? 'PASS' : 'FAIL');
console.log('0o7_7_7 === 0o777:', 0o7_7_7 === 0o777 ? 'PASS' : 'FAIL');

console.log('\n=== All numeric separator tests completed ===');
