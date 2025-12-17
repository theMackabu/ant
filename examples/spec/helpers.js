const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const RESET = '\x1b[0m';

let passed = 0;
let failed = 0;

export function test(name, actual, expected) {
  if (actual === expected) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    passed++;
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${expected}`);
    console.log(`  Actual:   ${actual}`);
    failed++;
  }
}

export function testApprox(name, actual, expected, epsilon = 1e-10) {
  const pass = (Number.isNaN(actual) && Number.isNaN(expected)) ||
    (!Number.isFinite(actual) && !Number.isFinite(expected) && actual === expected) ||
    Math.abs(actual - expected) < epsilon;
  if (pass) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    passed++;
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${expected}`);
    console.log(`  Actual:   ${actual}`);
    failed++;
  }
}

export function testThrows(name, fn) {
  try {
    fn();
    console.log(`${RED}✗${RESET} ${name} (expected to throw)`);
    failed++;
  } catch (e) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    passed++;
  }
}

export function testDeep(name, actual, expected) {
  const pass = JSON.stringify(actual) === JSON.stringify(expected);
  if (pass) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    passed++;
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${JSON.stringify(expected)}`);
    console.log(`  Actual:   ${JSON.stringify(actual)}`);
    failed++;
  }
}

export function summary() {
  console.log(`\nPassed: ${passed}`);
  console.log(`Failed: ${failed}`);
  if (failed > 0) process.exit(1);
}
