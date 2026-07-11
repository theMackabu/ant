function prefixInc(value) {
  return ++value;
}

function prefixDec(value) {
  return --value;
}

function postfixDec(value) {
  const previous = value--;
  return previous * 1000 + value;
}

for (let i = 0; i < 500; i++) {
  if (prefixInc(i) !== i + 1) throw new Error("prefix INC mismatch");
  if (prefixDec(i) !== i - 1) throw new Error("prefix DEC mismatch");
  if (postfixDec(i) !== i * 1000 + i - 1)
    throw new Error("postfix DEC mismatch");
}

if (!Number.isNaN(prefixInc(undefined)))
  throw new Error("prefix INC bailout mismatch");
if (!Number.isNaN(prefixDec(undefined)))
  throw new Error("prefix DEC bailout mismatch");
if (!Number.isNaN(postfixDec(undefined)))
  throw new Error("postfix DEC bailout mismatch");

console.log("OK: test_jit_inc_dec");
