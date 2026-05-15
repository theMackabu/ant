function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(label + ": expected " + JSON.stringify(expected) +
      ", got " + JSON.stringify(actual) + " (" + typeof actual + ")");
  }
}

function sparseCapturedParam(a, b, x) {
  const g = () => a;
  const y = x + 1;
  return b;
}

for (let i = 0; i < 300; i++) sparseCapturedParam(1, 123, 2);
assertEq(
  sparseCapturedParam(1, "SENTINEL-B", "x"),
  "SENTINEL-B",
  "bailout preserves non-captured sparse param"
);

function sparseCapturedParamWithWrite(a, b, x) {
  const g = () => a;
  b = b + "-written";
  const y = x + 1;
  return b;
}

for (let i = 0; i < 300; i++) sparseCapturedParamWithWrite(1, "warm", 2);
assertEq(
  sparseCapturedParamWithWrite(1, "SENTINEL-B", "x"),
  "SENTINEL-B-written",
  "bailout preserves updated non-captured sparse param"
);

console.log("OK: test_jit_sparse_captured_param_bailout");
