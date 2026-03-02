const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const RESET = '\x1b[0m';

let passed = 0;
let failed = 0;
let failures = [];

function specHook() {
  return globalThis.__ANT_SPEC_HOOK__ || null;
}

function specState() {
  const hook = specHook();
  if (hook && hook.current) return hook.current;
  return null;
}

function incPassed() {
  const s = specState();
  if (s) s.passed++;
  else passed++;
}

function incFailed(name) {
  const s = specState();
  if (s) {
    s.failed++;
    if (name) s.failures.push(`${RED}✗${RESET} ${name}`);
  } else {
    failed++;
    if (name) failures.push(`${RED}✗${RESET} ${name}`);
  }
}

export function test(name, actual, expected) {
  if (actual === expected) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    incPassed();
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${expected}`);
    console.log(`  Actual:   ${actual}`);
    incFailed(name);
  }
}

export function testApprox(name, actual, expected, epsilon = 1e-10) {
  const pass =
    (Number.isNaN(actual) && Number.isNaN(expected)) ||
    (!Number.isFinite(actual) && !Number.isFinite(expected) && actual === expected) ||
    Math.abs(actual - expected) < epsilon;
  if (pass) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    incPassed();
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${expected}`);
    console.log(`  Actual:   ${actual}`);
    incFailed(name);
  }
}

export function testThrows(name, fn) {
  try {
    fn();
    console.log(`${RED}✗${RESET} ${name} (expected to throw)`);
    incFailed(`${name} (expected to throw)`);
  } catch (e) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    incPassed();
  }
}

export function testDeep(name, actual, expected) {
  const pass = JSON.stringify(actual) === JSON.stringify(expected);
  if (pass) {
    console.log(`${GREEN}✓${RESET} ${name}`);
    incPassed();
  } else {
    console.log(`${RED}✗${RESET} ${name}`);
    console.log(`  Expected: ${JSON.stringify(expected)}`);
    console.log(`  Actual:   ${JSON.stringify(actual)}`);
    incFailed(name);
  }
}

export function summary() {
  const s = specState();
  const p = s ? s.passed : passed;
  const f = s ? s.failed : failed;

  console.log(`\nPassed: ${p}`);
  console.log(`Failed: ${f}`);

  const hook = specHook();
  if (hook && hook.current && typeof hook.onSummary === 'function') {
    hook.onSummary({
      passed: p,
      failed: f,
      failures: hook.current.failures.slice()
    });
    return;
  }

  if (f > 0) process.exit(1);
}
