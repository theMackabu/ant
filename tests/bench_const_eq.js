// Benchmark: constant eq/seq inline — measures overhead of calling
// jit_helper_eq/jit_helper_seq when one operand is a compile-time
// constant (true, false, undefined, null) vs. an inline beq/bne.

function bench(name, fn, iterations) {
  const start = Date.now();
  fn(iterations);
  const elapsed = Date.now() - start;
  console.log(name + ": " + elapsed + "ms (" + iterations + " iters, " + (elapsed / iterations * 1000).toFixed(2) + "µs/op)");
}

// 1. === true (most common pattern: result === true)
bench("=== true", function(n) {
  let count = 0;
  let v = true;
  for (let i = 0; i < n; i++)
    if (v === true) count++;
  return count;
}, 10000000);

// 2. === false
bench("=== false", function(n) {
  let count = 0;
  let v = false;
  for (let i = 0; i < n; i++)
    if (v === false) count++;
  return count;
}, 10000000);

// 3. === undefined
bench("=== undefined", function(n) {
  let count = 0;
  let v = undefined;
  for (let i = 0; i < n; i++)
    if (v === undefined) count++;
  return count;
}, 10000000);

// 4. === null
bench("=== null", function(n) {
  let count = 0;
  let v = null;
  for (let i = 0; i < n; i++)
    if (v === null) count++;
  return count;
}, 10000000);

// 5. == null (coercing — catches both null and undefined)
bench("== null", function(n) {
  let count = 0;
  let v = undefined;
  for (let i = 0; i < n; i++)
    if (v == null) count++;
  return count;
}, 10000000);

// 6. Branching on === true/false (switch-like pattern)
bench("branch true/false/other", function(n) {
  let sum = 0;
  let vals = [true, false, 42];
  for (let i = 0; i < n; i++) {
    let v = vals[i % 3];
    if (v === true) sum += 1;
    else if (v === false) sum += 2;
    else sum += 3;
  }
  return sum;
}, 5000000);

// 7. Optional field check (obj.x === undefined)
bench("field === undefined", function(n) {
  let o = { a: 1 };
  let count = 0;
  for (let i = 0; i < n; i++)
    if (o.b === undefined) count++;
  return count;
}, 5000000);

// 8. Truthiness after === (the truthy-dance pattern from newt)
bench("(x === y) === true", function(n) {
  let count = 0;
  let a = 5, b = 5;
  for (let i = 0; i < n; i++)
    if ((a === b) === true) count++;
  return count;
}, 10000000);

// 9. !== undefined guard
bench("!== undefined", function(n) {
  let count = 0;
  let v = 42;
  for (let i = 0; i < n; i++)
    if (v !== undefined) count++;
  return count;
}, 10000000);

// 10. Mixed constant comparisons in one function body
bench("mixed const cmp", function(n) {
  let sum = 0;
  let t = true, f = false, u = undefined, z = null;
  for (let i = 0; i < n; i++) {
    if (t === true) sum++;
    if (f === false) sum++;
    if (u === undefined) sum++;
    if (z === null) sum++;
  }
  return sum;
}, 5000000);
