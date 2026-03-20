// Benchmark: property read paths (lkp_val vs lkp + propref)

function bench(name, fn, iterations) {
  const start = Date.now();
  fn(iterations);
  const elapsed = Date.now() - start;
  console.log(name + ": " + elapsed + "ms (" + iterations + " iters, " + (elapsed / iterations * 1000).toFixed(2) + "µs/op)");
}

// 1. Global variable read (sv_global_get_interned)
var gval = 42;
bench("global read", function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += gval;
  return sum;
}, 5000000);

// 2. Object property read
bench("prop read", function(n) {
  let o = { x: 1, y: 2, z: 3 };
  let sum = 0;
  for (let i = 0; i < n; i++) sum += o.x + o.y + o.z;
  return sum;
}, 5000000);

// 3. Prototype chain lookup
bench("proto chain read", function(n) {
  function Foo() {}
  let o = new Foo();
  let sum = 0;
  for (let i = 0; i < n; i++) sum += o.constructor ? 1 : 0;
  return sum;
}, 1000000);

// 4. Array .length read
bench("array length read", function(n) {
  let arr = [1, 2, 3, 4, 5];
  let sum = 0;
  for (let i = 0; i < n; i++) sum += arr.length;
  return sum;
}, 5000000);

// 5. Function .name read
bench("func name read", function(n) {
  function myFunc() {}
  let count = 0;
  for (let i = 0; i < n; i++) count += myFunc.name.length;
  return count;
}, 1000000);

// 6. Repeated prop reads on large object
bench("10-prop obj repeated read", function(n) {
  let o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10 };
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += o.a + o.b + o.c + o.d + o.e + o.f + o.g + o.h + o.i + o.j;
  }
  return sum;
}, 1000000);

// 7. Constructor prototype lookup (string method)
bench("ctor proto lookup", function(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    let s = "hello";
    sum += s.length;
  }
  return sum;
}, 5000000);
