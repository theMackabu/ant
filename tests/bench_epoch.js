// Benchmark: IC epoch hoist — measures benefit of loading the global
// IC epoch once per function instead of once per GET_FIELD site.

function bench(name, fn, iterations) {
  const start = Date.now();
  fn(iterations);
  const elapsed = Date.now() - start;
  console.log(name + ": " + elapsed + "ms (" + iterations + " iters, " + (elapsed / iterations * 1000).toFixed(2) + "µs/op)");
}

// 1. Many IC sites on the same object (wide read)
bench("10-field single obj", function(n) {
  let o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10 };
  let sum = 0;
  for (let i = 0; i < n; i++)
    sum += o.a + o.b + o.c + o.d + o.e + o.f + o.g + o.h + o.i + o.j;
  return sum;
}, 2000000);

// 2. Chained field access — each dot is a separate IC site
bench("chained field 4-deep", function(n) {
  let o = { x: { y: { z: { w: 42 } } } };
  let sum = 0;
  for (let i = 0; i < n; i++) sum += o.x.y.z.w;
  return sum;
}, 5000000);

// 3. Multiple objects, multiple fields per iteration
bench("3-obj x 3-field", function(n) {
  let a = { x: 1, y: 2, z: 3 };
  let b = { x: 4, y: 5, z: 6 };
  let c = { x: 7, y: 8, z: 9 };
  let sum = 0;
  for (let i = 0; i < n; i++)
    sum += a.x + a.y + a.z + b.x + b.y + b.z + c.x + c.y + c.z;
  return sum;
}, 2000000);

// 4. Pattern matching style — branch with many IC reads
bench("branchy multi-field", function(n) {
  let o = { type: 1, value: 10, name: 20, data: 30, extra: 40 };
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (o.type === 1)
      sum += o.value + o.name;
    else
      sum += o.data + o.extra;
  }
  return sum;
}, 5000000);

// 5. Prototype chain with many field reads (IC + proto walk)
bench("proto 6-field", function(n) {
  function Point(x, y, z) { this.x = x; this.y = y; this.z = z; }
  Point.prototype.r = 10;
  Point.prototype.g = 20;
  Point.prototype.b = 30;
  let p = new Point(1, 2, 3);
  let sum = 0;
  for (let i = 0; i < n; i++)
    sum += p.x + p.y + p.z + p.r + p.g + p.b;
  return sum;
}, 2000000);

// 6. Non-loop straight-line many IC sites (function call overhead)
function readAll(o) {
  return o.a + o.b + o.c + o.d + o.e + o.f + o.g + o.h;
}
bench("call 8-field fn", function(n) {
  let o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 };
  let sum = 0;
  for (let i = 0; i < n; i++) sum += readAll(o);
  return sum;
}, 2000000);
