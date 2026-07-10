function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function coalesce(value, fallback) {
  return value ?? fallback;
}

function coalesceLazy(value, state) {
  return value ?? state.fallback();
}

function assignLocal(value) {
  let target = value;
  target ??= 42;
  return target;
}

function assignField(obj) {
  obj.value ??= 42;
  return obj.value;
}

function assignElem(obj, key) {
  obj[key] ??= 42;
  return obj[key];
}

function coalesceViaInline(value, fallback) {
  return coalesce(value, fallback);
}

const lazyState = {
  calls: 0,
  fallback() {
    this.calls++;
    return 42;
  },
};

for (let i = 0; i < 500; i++) {
  assertEq(coalesce(i, 42), i, "warm taken branch");
  assertEq(coalesceLazy(i, lazyState), i, "warm lazy taken branch");
  assertEq(assignLocal(i), i, "warm local assignment");
  assertEq(assignField({ value: i }), i, "warm field assignment");
  assertEq(assignElem({ value: i }, "value"), i, "warm element assignment");
  assertEq(coalesceViaInline(i, 42), i, "warm inline branch");
}

assertEq(lazyState.calls, 0, "taken branch skips fallback");
assertEq(coalesce(7, 42), 7, "hot number taken branch");
assertEq(coalesce(false, true), false, "hot false taken branch");
assertEq(coalesce(0, 42), 0, "hot zero taken branch");
assertEq(coalesce("", "fallback"), "", "hot empty string taken branch");
assertEq(coalesce(null, 42), 42, "hot null fallthrough");
assertEq(coalesce(undefined, 42), 42, "hot undefined fallthrough");

assertEq(coalesceLazy(null, lazyState), 42, "hot lazy null fallthrough");
assertEq(lazyState.calls, 1, "null evaluates fallback once");
assertEq(coalesceLazy(undefined, lazyState), 42, "hot lazy undefined fallthrough");
assertEq(lazyState.calls, 2, "undefined evaluates fallback once");

assertEq(assignLocal(null), 42, "hot local null assignment");
assertEq(assignLocal(undefined), 42, "hot local undefined assignment");
assertEq(assignLocal(0), 0, "hot local zero preservation");
assertEq(assignField({ value: null }), 42, "hot field null assignment");
assertEq(assignField({ value: 0 }), 0, "hot field zero preservation");
assertEq(assignElem({ value: undefined }, "value"), 42, "hot element undefined assignment");
assertEq(assignElem({ value: false }, "value"), false, "hot element false preservation");

assertEq(coalesceViaInline(7, 42), 7, "hot inline taken branch");
assertEq(coalesceViaInline(null, 42), 42, "hot inline null fallthrough");
assertEq(coalesceViaInline(undefined, 42), 42, "hot inline undefined fallthrough");

console.log("OK: test_jit_jmp_not_nullish");
