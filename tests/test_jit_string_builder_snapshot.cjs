function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(`${label}: got ${actual}, expected ${expected}`);
  }
}

function localSnapshot() {
  let value = '';
  for (let i = 0; i < 8; i++) value += 'x';

  const snapshot = value;
  value += 'y';
  return snapshot + ':' + value;
}

function parameterSnapshot(value) {
  for (let i = 0; i < 8; i++) value += 'x';

  const snapshot = value;
  value += 'y';
  return snapshot + ':' + value;
}

function capturedSnapshot() {
  let value = '';
  function read() {
    return value;
  }

  for (let i = 0; i < 8; i++) value += 'x';

  const snapshot = read();
  value += 'y';
  return snapshot + ':' + value;
}

// A parameter the function reassigns but never appends to must still
// snapshot correctly (reads of it carry no builder check).
function reassignedParameter(value) {
  let acc = '';
  for (let i = 0; i < 8; i++) acc += 'x';
  value = acc;

  const snapshot = value;
  acc += 'y';
  return snapshot + ':' + acc;
}

function unicodeSnapshot() {
  let value = '';
  for (let i = 0; i < 8; i++) value += 'é𝄞';

  const snapshot = value;
  value += '中';
  return snapshot.length + ':' + value.length + ':' + (snapshot === value ? 'aliased' : 'ok');
}

function storeAliasSnapshot() {
  let value = '';
  for (let i = 0; i < 8; i++) value += 'x';

  const box = [value];
  const obj = { v: value };
  value += 'y';
  return box[0] + ':' + obj.v + ':' + value;
}

const expected = 'xxxxxxxx:xxxxxxxxy';
for (let i = 0; i < 300; i++) {
  assertEq(localSnapshot(), expected, `hot local snapshot ${i}`);
  assertEq(parameterSnapshot(''), expected, `hot parameter snapshot ${i}`);
  assertEq(capturedSnapshot(), expected, `hot captured snapshot ${i}`);
  assertEq(reassignedParameter(''), expected, `hot reassigned parameter ${i}`);
  assertEq(unicodeSnapshot(), '24:25:ok', `hot unicode snapshot ${i}`);
  assertEq(storeAliasSnapshot(), 'xxxxxxxx:xxxxxxxx:xxxxxxxxy', `hot store alias ${i}`);
}

function osrSnapshot() {
  let value = '';
  let snapshot = '';

  for (let i = 0; i < 8_000; i++) {
    value += 'x';
    if (i === 1_999) snapshot = value;
  }

  return snapshot.length + ':' + value.length;
}

assertEq(osrSnapshot(), '2000:8000', 'OSR local snapshot');

// Numeric-only type feedback must not specialize a local that a cold branch
// can turn into a string builder: warm up with the append branch never taken,
// then take it in JIT'd code and read the local at the loop head.
function coldBranchAppend(flag, n) {
  let value = 0;
  for (let i = 0; i < n; i++) {
    if (flag && i === n - 1) return value;
    if (flag) value += 'x';
  }
  return value;
}

for (let i = 0; i < 300; i++) coldBranchAppend(false, 20);
assertEq(coldBranchAppend(true, 3), '0xx', 'cold-branch append return');

function coldBranchAppendSnapshot(flag, n) {
  let value = 0;
  let snapshot = null;
  for (let i = 0; i < n; i++) {
    snapshot = value;
    if (flag) value += 'x';
  }
  return snapshot + ':' + value;
}

for (let i = 0; i < 300; i++) coldBranchAppendSnapshot(false, 20);
assertEq(coldBranchAppendSnapshot(true, 3), '0xx:0xxx', 'cold-branch append snapshot');

console.log('OK: test_jit_string_builder_snapshot');
