// Test Number.prototype.toString with radix parameter

console.log("Testing Number.prototype.toString(radix):");

// Base 16 (hexadecimal)
console.log("\nHexadecimal (base 16):");
console.log("(0).toString(16):", (0).toString(16), "expected: 0");
console.log("(10).toString(16):", (10).toString(16), "expected: a");
console.log("(15).toString(16):", (15).toString(16), "expected: f");
console.log("(16).toString(16):", (16).toString(16), "expected: 10");
console.log("(255).toString(16):", (255).toString(16), "expected: ff");
console.log("(256).toString(16):", (256).toString(16), "expected: 100");
console.log("(4095).toString(16):", (4095).toString(16), "expected: fff");
console.log("(65535).toString(16):", (65535).toString(16), "expected: ffff");

// Base 2 (binary)
console.log("\nBinary (base 2):");
console.log("(0).toString(2):", (0).toString(2), "expected: 0");
console.log("(1).toString(2):", (1).toString(2), "expected: 1");
console.log("(2).toString(2):", (2).toString(2), "expected: 10");
console.log("(8).toString(2):", (8).toString(2), "expected: 1000");
console.log("(16).toString(2):", (16).toString(2), "expected: 10000");
console.log("(255).toString(2):", (255).toString(2), "expected: 11111111");

// Base 8 (octal)
console.log("\nOctal (base 8):");
console.log("(0).toString(8):", (0).toString(8), "expected: 0");
console.log("(7).toString(8):", (7).toString(8), "expected: 7");
console.log("(8).toString(8):", (8).toString(8), "expected: 10");
console.log("(64).toString(8):", (64).toString(8), "expected: 100");
console.log("(511).toString(8):", (511).toString(8), "expected: 777");

// Base 10 (decimal - default)
console.log("\nDecimal (base 10):");
console.log("(123).toString():", (123).toString(), "expected: 123");
console.log("(123).toString(10):", (123).toString(10), "expected: 123");

// Base 36 (maximum)
console.log("\nBase 36:");
console.log("(35).toString(36):", (35).toString(36), "expected: z");
console.log("(36).toString(36):", (36).toString(36), "expected: 10");

// Negative numbers
console.log("\nNegative numbers:");
console.log("(-10).toString(16):", (-10).toString(16), "expected: -a");
console.log("(-255).toString(16):", (-255).toString(16), "expected: -ff");
console.log("(-8).toString(2):", (-8).toString(2), "expected: -1000");

// Special values
console.log("\nSpecial values:");
console.log("(NaN).toString(16):", (NaN).toString(16), "expected: NaN");
console.log("(Infinity).toString(16):", (Infinity).toString(16), "expected: Infinity");
console.log("(-Infinity).toString(16):", (-Infinity).toString(16), "expected: -Infinity");

// parseInt with radix
console.log("\n--- parseInt with radix ---");

// Hexadecimal
console.log("\nparseInt hexadecimal:");
console.log("parseInt('0', 16):", parseInt('0', 16), "expected: 0");
console.log("parseInt('a', 16):", parseInt('a', 16), "expected: 10");
console.log("parseInt('A', 16):", parseInt('A', 16), "expected: 10");
console.log("parseInt('f', 16):", parseInt('f', 16), "expected: 15");
console.log("parseInt('ff', 16):", parseInt('ff', 16), "expected: 255");
console.log("parseInt('FF', 16):", parseInt('FF', 16), "expected: 255");
console.log("parseInt('100', 16):", parseInt('100', 16), "expected: 256");
console.log("parseInt('ffff', 16):", parseInt('ffff', 16), "expected: 65535");

// Binary
console.log("\nparseInt binary:");
console.log("parseInt('0', 2):", parseInt('0', 2), "expected: 0");
console.log("parseInt('1', 2):", parseInt('1', 2), "expected: 1");
console.log("parseInt('10', 2):", parseInt('10', 2), "expected: 2");
console.log("parseInt('1000', 2):", parseInt('1000', 2), "expected: 8");
console.log("parseInt('11111111', 2):", parseInt('11111111', 2), "expected: 255");

// Octal
console.log("\nparseInt octal:");
console.log("parseInt('0', 8):", parseInt('0', 8), "expected: 0");
console.log("parseInt('7', 8):", parseInt('7', 8), "expected: 7");
console.log("parseInt('10', 8):", parseInt('10', 8), "expected: 8");
console.log("parseInt('100', 8):", parseInt('100', 8), "expected: 64");
console.log("parseInt('777', 8):", parseInt('777', 8), "expected: 511");

// Base 36
console.log("\nparseInt base 36:");
console.log("parseInt('z', 36):", parseInt('z', 36), "expected: 35");
console.log("parseInt('Z', 36):", parseInt('Z', 36), "expected: 35");
console.log("parseInt('10', 36):", parseInt('10', 36), "expected: 36");

// Default base 10
console.log("\nparseInt decimal:");
console.log("parseInt('123'):", parseInt('123'), "expected: 123");
console.log("parseInt('123', 10):", parseInt('123', 10), "expected: 123");

// Validation tests
console.log("\n--- Validation ---");
let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    passed++;
  } else {
    failed++;
    console.log("FAIL:", name, "got:", actual, "expected:", expected);
  }
}

// toString tests
test("toString hex 0", (0).toString(16), "0");
test("toString hex 10", (10).toString(16), "a");
test("toString hex 15", (15).toString(16), "f");
test("toString hex 255", (255).toString(16), "ff");
test("toString bin 8", (8).toString(2), "1000");
test("toString bin 255", (255).toString(2), "11111111");
test("toString oct 64", (64).toString(8), "100");
test("toString base36 35", (35).toString(36), "z");
test("toString neg hex", (-15).toString(16), "-f");
test("toString decimal default", (123).toString(), "123");

// parseInt tests
test("parseInt hex ff", parseInt('ff', 16), 255);
test("parseInt hex FF", parseInt('FF', 16), 255);
test("parseInt bin 1010", parseInt('1010', 2), 10);
test("parseInt oct 777", parseInt('777', 8), 511);
test("parseInt base36 z", parseInt('z', 36), 35);
test("parseInt base36 Z", parseInt('Z', 36), 35);
test("parseInt decimal", parseInt('123', 10), 123);
test("parseInt default", parseInt('456'), 456);

// Round-trip tests (toString then parseInt)
test("roundtrip hex 255", parseInt((255).toString(16), 16), 255);
test("roundtrip bin 42", parseInt((42).toString(2), 2), 42);
test("roundtrip oct 100", parseInt((100).toString(8), 8), 100);
test("roundtrip base36 1000", parseInt((1000).toString(36), 36), 1000);

console.log("\nResults:", passed, "passed,", failed, "failed");

if (failed === 0) {
  console.log("All radix tests passed!");
}
