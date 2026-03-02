let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log("PASS:", name);
    passed++;
    return;
  }
  console.log("FAIL:", name, "- expected", expected, "got", actual);
  failed++;
}

const ctx = { total: 0, seen: "" };
[1, 2, 3].forEach(function (n) {
  this.total += n;
  this.seen += String(n);
}, ctx);
test("forEach thisArg", ctx.total, 6);
test("forEach callback this binding", ctx.seen, "123");

const mapped = [1, 2].map(function (n) {
  return this.base + n;
}, { base: 10 });
test("map thisArg", mapped[0], 11);
test("map thisArg second value", mapped[1], 12);

const found = [1, 2, 3].find(function (n) {
  return n > this.min;
}, { min: 2 });
test("find thisArg", found, 3);

console.log("Passed:", passed);
console.log("Failed:", failed);

if (failed > 0) throw new Error("test_array_callback_thisarg failed");
