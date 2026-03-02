const N = 500000;

function bench(name, fn) {
  let start = Date.now();
  let result = fn();
  let elapsed = Date.now() - start;
  return { name, result, elapsed };
}

let t1_start = Date.now();
let sum = 0;
for (let i = 0; i < N; i++) {
  sum = sum + i;
}
let t1_ms = Date.now() - t1_start;
let t1_expected = ((N - 1) * N) / 2;
console.log(`[test1] top-level loop:       ${t1_ms}ms  sum=${sum}  ok=${sum === t1_expected}`);

function computeOnce(n) {
  let total = 0;
  for (let j = 0; j < n; j++) {
    total = total + j;
  }
  return total;
}

let t2 = bench('once-called fn', () => computeOnce(N));
let t2_expected = ((N - 1) * N) / 2;
console.log(`[test2] ${t2.name}:    ${t2.elapsed}ms  result=${t2.result}  ok=${t2.result === t2_expected}`);

function nestedLoops(outer, inner) {
  let acc = 0;
  for (let a = 0; a < outer; a++) {
    for (let b = 0; b < inner; b++) acc = acc + 1;
  }
  return acc;
}

let t3 = bench('nested loops', () => nestedLoops(20, N / 20));
let t3_expected = 20 * (N / 20);
console.log(`[test3] ${t3.name}:       ${t3.elapsed}ms  result=${t3.result}  ok=${t3.result === t3_expected}`);

function add(a, b) {
  return a + b;
}

function loopWithCall(n) {
  let s = 0;
  for (let k = 0; k < n; k++) {
    s = add(s, k);
  }
  return s;
}

let t4 = bench('loop + JIT callee', () => loopWithCall(N));
let t4_expected = ((N - 1) * N) / 2;
console.log(`[test4] ${t4.name}: ${t4.elapsed}ms  result=${t4.result}  ok=${t4.result === t4_expected}`);

function whileLoop(n) {
  let count = 0;
  let x = 0;
  while (x < n) {
    count = count + 1;
    x = x + 1;
  }
  return count;
}

let t5 = bench('while loop', () => whileLoop(N));
console.log(`[test5] ${t5.name}:        ${t5.elapsed}ms  result=${t5.result}  ok=${t5.result === N}`);

function localPreservation(n) {
  let a = 42;
  let b = 99;
  let c = 0;
  for (let i = 0; i < n; i++) {
    c = c + 1;
  }
  return a + b + c;
}

let t6 = bench('local preservation', () => localPreservation(N));
let t6_expected = 42 + 99 + N;
console.log(`[test6] ${t6.name}: ${t6.elapsed}ms  result=${t6.result}  ok=${t6.result === t6_expected}`);

function hotAdd(a, b) {
  return a + b;
}

for (let w = 0; w < 110; w++) hotAdd(w, w);
let t7_call = bench('hot fn (call_count JIT)', () => {
  let s = 0;
  for (let i = 0; i < N; i++) s = hotAdd(s, i);
  return s;
});

let t7_osr = bench('cold fn (OSR JIT)', () => {
  function coldLoop(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s = s + i;
    return s;
  }
  return coldLoop(N);
});

console.log(`[test7] ${t7_call.name}:  ${t7_call.elapsed}ms`);
console.log(`[test7] ${t7_osr.name}:         ${t7_osr.elapsed}ms`);
console.log(`[test7] results match: ${t7_call.result === t7_osr.result}`);

let allOk =
  sum === t1_expected &&
  t2.result === t2_expected &&
  t3.result === t3_expected &&
  t4.result === t4_expected &&
  t5.result === N &&
  t6.result === t6_expected &&
  t7_call.result === t7_osr.result;

console.log('');
console.log('=== timing summary ===');
console.log(`  top-level loop:       ${t1_ms}ms`);
console.log(`  once-called fn:       ${t2.elapsed}ms`);
console.log(`  nested loops:         ${t3.elapsed}ms`);
console.log(`  loop + JIT callee:    ${t4.elapsed}ms`);
console.log(`  while loop:           ${t5.elapsed}ms`);
console.log(`  local preservation:   ${t6.elapsed}ms`);
console.log(`  hot fn (call JIT):    ${t7_call.elapsed}ms`);
console.log(`  cold fn (OSR JIT):    ${t7_osr.elapsed}ms`);
console.log('');
console.log('all OSR tests passed:', allOk);
