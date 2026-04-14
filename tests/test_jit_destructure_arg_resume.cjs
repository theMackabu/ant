function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(msg + " (expected " + expected + ", got " + actual + ")");
  }
}

function makeValues(count, first) {
  const values = new Array(count);
  for (let i = 0; i < count; i++) values[i] = i;
  values[0] = first;
  return values;
}

function makeTarget(paramCount) {
  const params = [];
  let source = "";

  for (let i = 0; i < paramCount; i++) {
    params.push("a" + i);
  }

  source += "function target(" + params.join(",") + ") {";
  source += "  let [x] = [1, 2];";
  source += "  return a0;";
  source += "}";
  source += "return target;";
  return Function(source)();
}

function runCase(label, paramCount) {
  const target = makeTarget(paramCount);
  const warm = makeValues(paramCount, "warm");

  for (let i = 0; i < 200; i++) {
    const got = target.apply(undefined, warm);
    assertEq(got, "warm", label + " should preserve arg reads during warmup");
  }

  const magic = makeValues(paramCount, "MAGIC");
  const got = target.apply(undefined, magic);
  assertEq(got, "MAGIC", label + " should preserve arg reads after destructuring");
}

runCase("high-arity apply call", 242);
runCase("slightly-lower-arity apply call", 241);

console.log("OK: test_jit_destructure_arg_resume");
