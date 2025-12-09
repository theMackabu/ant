// Test Atomics API with Binary, Octal, and Hex literals
console.log('=== Atomics with Number Literals Test ===\n');

const sab = new SharedArrayBuffer(1024);
const ta = new Uint8Array(sab);

console.log('Test 1: Initialize with binary literal');
Atomics.store(ta, 0, 0b00000000);
console.log('Stored 0b00000000:', Atomics.load(ta, 0));
console.log('PASS:', Atomics.load(ta, 0) === 0);

console.log('\nTest 2: Add using hex literal');
const old1 = Atomics.add(ta, 0, 0xFF);
console.log('Added 0xFF to 0, old value:', old1);
console.log('New value:', Atomics.load(ta, 0));
console.log('PASS:', Atomics.load(ta, 0) === 0xFF);

console.log('\nTest 3: AND with binary mask');
Atomics.store(ta, 1, 0b11111111);
const old2 = Atomics.and(ta, 1, 0b11110000);
console.log('0b11111111 & 0b11110000 =', Atomics.load(ta, 1));
console.log('PASS:', Atomics.load(ta, 1) === 0b11110000);

console.log('\nTest 4: OR with hex value');
const old3 = Atomics.or(ta, 1, 0x0F);
console.log('0b11110000 | 0x0F =', Atomics.load(ta, 1));
console.log('PASS:', Atomics.load(ta, 1) === 0xFF);

console.log('\nTest 5: XOR with octal value');
Atomics.store(ta, 2, 0o377);  // 255 in octal
const old4 = Atomics.xor(ta, 2, 0b10101010);
console.log('0o377 ^ 0b10101010 =', Atomics.load(ta, 2));
console.log('PASS:', Atomics.load(ta, 2) === (0o377 ^ 0b10101010));

console.log('\nTest 6: CompareExchange with mixed literals');
Atomics.store(ta, 3, 0x10);
const result = Atomics.compareExchange(ta, 3, 0b00010000, 0o40);
console.log('Expected 0b00010000 (16), got:', result);
console.log('New value (0o40 = 32):', Atomics.load(ta, 3));
console.log('PASS:', result === 0x10 && Atomics.load(ta, 3) === 0o40);

console.log('\nTest 7: Bitwise flags using binary literals');
const FLAG_READ = 0b0001;
const FLAG_WRITE = 0b0010;
const FLAG_EXECUTE = 0b0100;
const FLAG_DELETE = 0b1000;

Atomics.store(ta, 4, 0);
Atomics.or(ta, 4, FLAG_READ);
Atomics.or(ta, 4, FLAG_WRITE);
const flags = Atomics.load(ta, 4);
console.log('Flags after OR READ|WRITE:', flags.toString(2).padStart(4, '0'));
console.log('Has READ:', (flags & FLAG_READ) !== 0);
console.log('Has WRITE:', (flags & FLAG_WRITE) !== 0);
console.log('Has EXECUTE:', (flags & FLAG_EXECUTE) !== 0);
console.log('PASS:', flags === 0b0011);

console.log('\nTest 8: Color manipulation with hex');
const RED = 0xFF;
const GREEN = 0xFF;
const BLUE = 0xFF;

Atomics.store(ta, 5, RED);
Atomics.store(ta, 6, GREEN);
Atomics.store(ta, 7, BLUE);

console.log('RGB values:');
console.log('  R:', Atomics.load(ta, 5));
console.log('  G:', Atomics.load(ta, 6));
console.log('  B:', Atomics.load(ta, 7));
console.log('PASS:', Atomics.load(ta, 5) === 255);

console.log('\nTest 9: Mask operations with all formats');
const MASK_BIN = 0b11110000;
const MASK_OCT = 0o360;  // Same as 0b11110000
const MASK_HEX = 0xF0;   // Same as 0b11110000

Atomics.store(ta, 8, 0xFF);
Atomics.and(ta, 8, MASK_BIN);
const result1 = Atomics.load(ta, 8);

Atomics.store(ta, 9, 0xFF);
Atomics.and(ta, 9, MASK_OCT);
const result2 = Atomics.load(ta, 9);

Atomics.store(ta, 10, 0xFF);
Atomics.and(ta, 10, MASK_HEX);
const result3 = Atomics.load(ta, 10);

console.log('All masks equivalent:', result1 === result2 && result2 === result3);
console.log('PASS:', result1 === 0xF0);

console.log('\nTest 10: Complex atomic operation chain');
Atomics.store(ta, 11, 0b00000000);
console.log('Start:', Atomics.load(ta, 11).toString(2).padStart(8, '0'));

Atomics.or(ta, 11, 0b00001111);
console.log('After OR 0b00001111:', Atomics.load(ta, 11).toString(2).padStart(8, '0'));

Atomics.and(ta, 11, 0b11110111);
console.log('After AND 0b11110111:', Atomics.load(ta, 11).toString(2).padStart(8, '0'));

Atomics.xor(ta, 11, 0xFF);
console.log('After XOR 0xFF:', Atomics.load(ta, 11).toString(2).padStart(8, '0'));

const final = Atomics.load(ta, 11);
console.log('PASS:', final === 0b11111000);

console.log('\n=== All tests completed successfully ===');
