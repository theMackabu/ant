let failed = 0;

function eq(name, actual, expected) {
  if (Object.is(actual, expected)) return;
  failed++;
  console.log(`FAIL ${name}: got ${actual}, want ${expected}`);
}

function voidComparison(a, b) {
  if (void (a === b)) return 'T';
  return 'F';
}

eq('void comparison cold', voidComparison(1, 2), 'F');
for (let i = 0; i < 1000; i++) {
  const actual = voidComparison(i, i);
  if (actual === 'F') continue;
  eq(`void comparison hot at call ${i + 2}`, actual, 'F');
  break;
}

if (failed) process.exit(1);
console.log('OK');
