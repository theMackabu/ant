const failures = [];

function assertOrder(name, got, expected) {
  const ok = got.length === expected.length && got.every((v, i) => v === expected[i]);
  if (!ok) failures.push(`${name}: expected [${expected.join(', ')}], got [${got.join(', ')}]`);
}

{
  const order = [];
  const base = {};
  const getObj = () => { order.push('obj'); return base; };
  const key = () => { order.push('key'); return 'k'; };
  const rhs = () => { order.push('rhs'); return 1; };
  getObj()[key()] = rhs();
  assertOrder('computed assignment order', order, ['obj', 'key', 'rhs']);
}

{
  const order = [];
  const base = {};
  const getObj = () => { order.push('obj'); return base; };
  const rhs = () => { order.push('rhs'); return 1; };
  getObj().x = rhs();
  assertOrder('named assignment order', order, ['obj', 'rhs']);
}

if (failures.length) {
  console.log('Assignment order failures:');
  for (let i = 0; i < failures.length; i++) {
    console.log(`  ${i + 1}. ${failures[i]}`);
  }
  throw new Error(`Found ${failures.length} assignment-order regression(s)`);
}

console.log('PASS: assignment evaluation order');
