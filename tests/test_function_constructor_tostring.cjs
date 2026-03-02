let failed = 0;

const bodyObj = {
  toString() {
    return "return x + 41;";
  },
};

const paramObj = {
  toString() {
    return "x";
  },
};

try {
  const fn = Function(paramObj, bodyObj);
  if (fn(1) !== 42) {
    failed++;
    console.log("direct Function() returned wrong value");
  }
} catch (e) {
  failed++;
  console.log("direct Function() threw:", e && e.message ? e.message : e);
}

try {
  const fn2 = Function.apply(null, [paramObj, bodyObj]);
  if (fn2(2) !== 43) {
    failed++;
    console.log("Function.apply() returned wrong value");
  }
} catch (e) {
  failed++;
  console.log("Function.apply() threw:", e && e.message ? e.message : e);
}

if (failed > 0) throw new Error("test_function_constructor_tostring failed");
