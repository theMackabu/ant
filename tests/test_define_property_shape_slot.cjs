function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(msg + " (expected " + expected + ", got " + actual + ")");
  }
}

console.log("defineProperty shape-slot regression");

console.log("\nTest 1: repeated fresh data-property definitions on one object");
{
  const obj = {};
  for (let i = 0; i < 64; i++) {
    Object.defineProperty(obj, "p" + i, {
      value: i,
      writable: true,
      enumerable: true,
      configurable: true,
    });
  }

  let sum = 0;
  for (let i = 0; i < 64; i++) sum += obj["p" + i];
  assertEq(sum, (63 * 64) / 2, "all fresh data properties should be readable");
  assertEq(Object.keys(obj).length, 64, "all enumerable fresh properties should be visible");
}
console.log("PASS");

console.log("\nTest 2: fresh non-enumerable property should not leak into keys");
{
  const obj = {};
  Object.defineProperty(obj, "hidden", {
    value: 123,
    enumerable: false,
    configurable: true,
    writable: true,
  });
  obj.visible = 456;

  assertEq(obj.hidden, 123, "hidden property should be readable");
  assertEq(obj.visible, 456, "normal property should still work");
  assertEq(Object.keys(obj).length, 1, "only visible should appear in keys");
  assertEq(Object.keys(obj)[0], "visible", "visible key should be preserved");
}
console.log("PASS");

console.log("\nTest 3: fresh accessor descriptors should keep getter/setter semantics");
{
  const obj = {};
  let backing = 10;

  Object.defineProperty(obj, "value", {
    get() { return backing + 1; },
    set(v) { backing = v * 2; },
    enumerable: true,
    configurable: true,
  });

  assertEq(obj.value, 11, "fresh getter should be active");
  obj.value = 7;
  assertEq(backing, 14, "fresh setter should be active");
  assertEq(obj.value, 15, "getter should still observe updated backing");
}
console.log("PASS");

console.log("\nTest 4: repeated fresh defineProperty on many objects");
{
  let total = 0;
  for (let i = 0; i < 2000; i++) {
    const obj = {};
    Object.defineProperty(obj, "x", {
      value: i,
      writable: true,
      enumerable: true,
      configurable: true,
    });
    Object.defineProperty(obj, "y", {
      value: i + 1,
      writable: true,
      enumerable: false,
      configurable: true,
    });
    total += obj.x + obj.y;
    assertEq(Object.keys(obj).length, 1, "fresh per-object enumerable shape should stay correct");
  }

  assert(total > 0, "loop total should accumulate");
}
console.log("PASS");

console.log("\nTest 5: defining then redefining adjacent fresh slots");
{
  const obj = {};
  Object.defineProperty(obj, "a", {
    value: 1,
    writable: true,
    enumerable: true,
    configurable: true,
  });
  Object.defineProperty(obj, "b", {
    value: 2,
    writable: true,
    enumerable: true,
    configurable: true,
  });
  Object.defineProperty(obj, "a", {
    value: 10,
    writable: true,
    enumerable: true,
    configurable: true,
  });

  assertEq(obj.a, 10, "redefining earlier property should not corrupt later fresh slot");
  assertEq(obj.b, 2, "later fresh slot should remain intact");
}
console.log("PASS");

console.log("\nAll shape-slot regression tests passed");
