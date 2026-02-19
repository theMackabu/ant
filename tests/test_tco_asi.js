// Regression: verify TCO behavior is preserved with semicolon-free (ASI) style
// and that newline-after-return keeps standard ASI semantics.

function fail(msg) {
  console.log("FAIL:", msg)
  process.exit(1)
}

function assertEq(actual, expected, msg) {
  if (actual !== expected) fail(msg + " (got " + actual + ", expected " + expected + ")")
}

console.log("=== TCO + ASI Regression ===")

// Semicolon-free style: this should still be optimized as a tail call.
function countDownAsi(n) {
  if (n === 0) return 0
  return countDownAsi(n - 1)
}

assertEq(countDownAsi(100000), 0, "tail recursion with ASI should complete")

// Tail recursion in ternary form with no trailing semicolons.
function sumAsi(n, acc = 0) {
  return n === 0 ? acc : sumAsi(n - 1, acc + n)
}

assertEq(sumAsi(100000), 5000050000, "tail recursion in ternary should complete")

// ASI rule check: newline after `return` ends the statement.
function returnNewlineAsi() {
  return
  42
}

if (returnNewlineAsi() !== undefined) fail("newline after return must produce undefined")

console.log("PASS")
