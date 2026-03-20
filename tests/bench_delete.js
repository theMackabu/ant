// Benchmark: delete operator performance

function bench(name, fn, iterations) {
  const start = Date.now();
  fn(iterations);
  const elapsed = Date.now() - start;
  console.log(name + ": " + elapsed + "ms (" + iterations + " iters, " + (elapsed / iterations * 1000).toFixed(2) + "µs/op)");
}

bench("delete single prop", function(n) {
  for (let i = 0; i < n; i++) {
    let o = { a: 1 };
    delete o.a;
  }
}, 5000);

bench("delete from 10-prop obj", function(n) {
  for (let i = 0; i < n; i++) {
    let o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10 };
    delete o.e;
  }
}, 5000);

bench("repeated delete (5 of 10)", function(n) {
  for (let i = 0; i < n; i++) {
    let o = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10 };
    delete o.a;
    delete o.b;
    delete o.c;
    delete o.d;
    delete o.e;
  }
}, 2000);

bench("delete + re-add cycle", function(n) {
  let o = { x: 1 };
  for (let i = 0; i < n; i++) {
    delete o.x;
    o.x = i;
  }
}, 5000);

bench("delete non-existent", function(n) {
  let o = { a: 1 };
  for (let i = 0; i < n; i++) {
    delete o.z;
  }
}, 5000);

bench("delete computed key", function(n) {
  for (let i = 0; i < n; i++) {
    let o = { a: 1, b: 2, c: 3 };
    delete o["b"];
  }
}, 5000);

bench("delete from 50-prop obj", function(n) {
  for (let i = 0; i < n; i++) {
    let o = {};
    for (let j = 0; j < 50; j++) o["k" + j] = j;
    delete o["k25"];
  }
}, 1000);

bench("delete 25 from 50-prop obj", function(n) {
  for (let i = 0; i < n; i++) {
    let o = {};
    for (let j = 0; j < 50; j++) o["k" + j] = j;
    for (let j = 0; j < 25; j++) delete o["k" + j];
  }
}, 500);

bench("baseline: prop set only", function(n) {
  for (let i = 0; i < n; i++) {
    let o = { a: 1, b: 2, c: 3, d: 4, e: 5 };
    o.f = 6;
  }
}, 5000);
