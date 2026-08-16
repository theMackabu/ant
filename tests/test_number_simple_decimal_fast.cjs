function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertNumberString(source, expected, message) {
  const actual = Number(source);
  assert(
    Object.is(actual, expected),
    `${message}: Number(${JSON.stringify(source)}) expected ${expected}, got ${actual}`,
  );
}

assertNumberString("12345", 12345, "integer");
assertNumberString("12345.6", 12345.6, "fraction");
assertNumberString("+1.25", 1.25, "positive sign");
assertNumberString("-0", -0, "negative zero");
assertNumberString("0.", 0, "trailing dot");
assertNumberString(".5", 0.5, "leading dot");
assertNumberString("0001.25", 1.25, "leading zeros");
assertNumberString("9007199254740991", 9007199254740991, "largest exact mantissa");

// These deliberately miss the simple path and exercise the unchanged parser.
assertNumberString("1e3", 1000, "exponent fallback");
assertNumberString(" 42 ", 42, "whitespace fallback");
assertNumberString("0x10", 16, "hex fallback");
assertNumberString("0b11", 3, "binary fallback");
assertNumberString("Infinity", Infinity, "Infinity fallback");
assertNumberString("", 0, "empty-string fallback");
assert(Number("not a number") !== Number("not a number"), "invalid string is NaN");

// parseFloat uses the established double-conversion parser. Compare thousands
// of dynamically built decimals so the fast path cannot hide a rounding error
// behind constants parsed by the compiler.
let state = 0x12345678;
for (let i = 0; i < 10000; i++) {
  state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
  const integer = state % 100000000;
  state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
  const fraction = state % 1000000;
  const sign = (state & 1) === 0 ? "" : "-";
  const source = `${sign}${integer}.${String(fraction).padStart(6, "0")}`;
  const fast = Number(source);
  const reference = parseFloat(source);
  assert(Object.is(fast, reference), `decimal mismatch for ${source}`);
}

let intSum = 0;
let floatSum = 0;
for (let i = 0; i < 500; i++) {
  intSum += "12345" | 0;
  floatSum -= "12345.6";
}
assert(intSum === 6172500, "bitwise string conversion");
assert(Math.abs(floatSum + 6172800) < 0.001, "arithmetic string conversion");

console.log("OK: test_number_simple_decimal_fast");
