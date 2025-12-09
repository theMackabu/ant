console.log('=== Number Literal Tests ===\n');

// Test 1: Binary literals
console.log('Test 1: Binary literals');
console.log('0b0:', 0b0);
console.log('0b1:', 0b1);
console.log('0b10:', 0b10);
console.log('0b1010:', 0b1010);
console.log('0b11111111:', 0b11111111);
console.log('Binary 0b1010 === 10:', 0b1010 === 10 ? 'PASS' : 'FAIL');
console.log('Binary 0b11111111 === 255:', 0b11111111 === 255 ? 'PASS' : 'FAIL');

// Test 2: Octal literals
console.log('\nTest 2: Octal literals');
console.log('0o0:', 0o0);
console.log('0o7:', 0o7);
console.log('0o10:', 0o10);
console.log('0o755:', 0o755);
console.log('0o777:', 0o777);
console.log('Octal 0o10 === 8:', 0o10 === 8 ? 'PASS' : 'FAIL');
console.log('Octal 0o755 === 493:', 0o755 === 493 ? 'PASS' : 'FAIL');

// Test 3: Hexadecimal literals
console.log('\nTest 3: Hexadecimal literals');
console.log('0x0:', 0x0);
console.log('0xF:', 0xF);
console.log('0x10:', 0x10);
console.log('0xFF:', 0xFF);
console.log('0xDEADBEEF:', 0xDEADBEEF);
console.log('Hex 0x10 === 16:', 0x10 === 16 ? 'PASS' : 'FAIL');
console.log('Hex 0xFF === 255:', 0xFF === 255 ? 'PASS' : 'FAIL');

// Test 4: Uppercase variants
console.log('\nTest 4: Uppercase variants');
console.log('0B1010:', 0B1010);
console.log('0O755:', 0O755);
console.log('0XFF:', 0XFF);
console.log('Uppercase 0B1010 === 10:', 0B1010 === 10 ? 'PASS' : 'FAIL');
console.log('Uppercase 0O755 === 493:', 0O755 === 493 ? 'PASS' : 'FAIL');
console.log('Uppercase 0XFF === 255:', 0XFF === 255 ? 'PASS' : 'FAIL');

// Test 5: Mixed usage in expressions
console.log('\nTest 5: Mixed usage in expressions');
const sum = 0b1010 + 0o12 + 0x0A;
console.log('0b1010 + 0o12 + 0x0A =', sum);
console.log('Sum === 30:', sum === 30 ? 'PASS' : 'FAIL');

const product = 0b10 * 0o10 * 0x10;
console.log('0b10 * 0o10 * 0x10 =', product);
console.log('Product === 256:', product === 256 ? 'PASS' : 'FAIL');

// Test 6: Bitwise operations with binary literals
console.log('\nTest 6: Bitwise operations with binary literals');
console.log('0b1111 & 0b1010 =', (0b1111 & 0b1010).toString(2).padStart(4, '0'));
console.log('0b1111 | 0b1010 =', (0b1111 | 0b1010).toString(2).padStart(4, '0'));
console.log('0b1111 ^ 0b1010 =', (0b1111 ^ 0b1010).toString(2).padStart(4, '0'));
console.log('AND result === 10:', (0b1111 & 0b1010) === 10 ? 'PASS' : 'FAIL');
console.log('OR result === 15:', (0b1111 | 0b1010) === 15 ? 'PASS' : 'FAIL');
console.log('XOR result === 5:', (0b1111 ^ 0b1010) === 5 ? 'PASS' : 'FAIL');

// Test 7: All representations of the same number
console.log('\nTest 7: All representations of the same number');
console.log('Binary 0b1111 === 15:', 0b1111 === 15);
console.log('Octal 0o17 === 15:', 0o17 === 15);
console.log('Hex 0xF === 15:', 0xF === 15);
console.log('Decimal 15 === 15:', 15 === 15);
console.log('All equal:', (0b1111 === 0o17 && 0o17 === 0xF && 0xF === 15) ? 'PASS' : 'FAIL');

console.log('\n=== All number literal tests completed ===');
