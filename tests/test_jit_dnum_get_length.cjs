let failed = 0;

function eq(name, actual, expected) {
  if (Object.is(actual, expected)) return;
  failed++;
  console.log(`FAIL ${name}: got ${actual}, want ${expected}`);
}

// OP_GET_LENGTH on a d-only (dnum) local: GET_SLOT_RAW pushes the value
// unboxed (SLOT_NUM), leaving the vstack slot's boxed reg holding whatever
// last occupied it. GET_LENGTH must rebox before reading the tag — here the
// stale reg holds the string s, so the miscompile returned s.length (12)
// instead of undefined.
function counterLength(s) {
  let u, r;
  for (let i = 0; i < 20; i++) {
    u = s;
    r = i.length;
  }
  return r;
}
for (let k = 0; k < 500; k++) {
  const r = counterLength('hello world!');
  if (r === undefined) continue;
  eq(`dnum local .length hot at call ${k + 1}`, r, undefined);
  break;
}

// Unboxed arithmetic result feeding GET_LENGTH directly.
function exprLength(x) {
  return (x * 1.5).length;
}
for (let k = 0; k < 500; k++) {
  const r = exprLength(k);
  if (r === undefined) continue;
  eq(`unboxed expr .length hot at call ${k + 1}`, r, undefined);
  break;
}

if (failed) process.exit(1);
console.log('OK');
