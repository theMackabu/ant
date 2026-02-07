const now = () => typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();

function bench(name, fn, iters = 1) {
  // warmup
  fn();
  const t0 = now();
  for (let i = 0; i < iters; i++) fn();
  const dt = now() - t0;
  const per = (dt / iters).toFixed(3);
  console.log(`${name}: ${dt.toFixed(2)} ms total, ${per} ms/iter (${iters} iters)`);
}

// -------------------------------------------------------
// 1. Sequential push — O(n) lkp per push makes this O(n²)
// -------------------------------------------------------
function push_n(n) {
  const arr = [];
  for (let i = 0; i < n; i++) arr.push(i);
  return arr;
}

bench('push 1k', () => push_n(1000), 100);
bench('push 5k', () => push_n(5000), 20);
bench('push 10k', () => push_n(10000), 5);

// -------------------------------------------------------
// 2. Index write — arr[i] = v walks property chain
// -------------------------------------------------------
function index_write(n) {
  const arr = [];
  for (let i = 0; i < n; i++) arr[i] = i;
  return arr;
}

bench('index write 1k', () => index_write(1000), 100);
bench('index write 5k', () => index_write(5000), 20);

// -------------------------------------------------------
// 3. Index read — arr[i] walks property chain from head
// -------------------------------------------------------
function index_read(arr, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += arr[i];
  return sum;
}

{
  const arr1k = push_n(1000);
  bench('index read 1k', () => index_read(arr1k, 1000), 200);
}
{
  const arr5k = push_n(5000);
  bench('index read 5k', () => index_read(arr5k, 5000), 20);
}

// -------------------------------------------------------
// 4. Random access — worst case for linked property chain
// -------------------------------------------------------
function random_read(arr, indices) {
  let sum = 0;
  for (let i = 0; i < indices.length; i++) sum += arr[indices[i]];
  return sum;
}

{
  const n = 2000;
  const arr = push_n(n);
  const indices = [];
  let s = 7;
  for (let i = 0; i < n; i++) {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    indices.push(s % n);
  }
  bench('random read 2k', () => random_read(arr, indices), 50);
}

// -------------------------------------------------------
// 5. Pop — must walk chain to find last element
// -------------------------------------------------------
function push_then_pop(n) {
  const arr = [];
  for (let i = 0; i < n; i++) arr.push(i);
  for (let i = 0; i < n; i++) arr.pop();
}

bench('push+pop 1k', () => push_then_pop(1000), 50);

// -------------------------------------------------------
// 6. Iteration — forEach / map / filter / reduce
// -------------------------------------------------------
{
  const arr1k = push_n(1000);

  bench('forEach 1k', () => {
    let s = 0;
    arr1k.forEach(x => s += x);
  }, 200);

  bench('map 1k', () => arr1k.map(x => x * 2), 100);
  bench('filter 1k', () => arr1k.filter(x => x % 2 === 0), 100);
  bench('reduce 1k', () => arr1k.reduce((a, b) => a + b, 0), 200);
}

// -------------------------------------------------------
// 7. Slice / concat / spread
// -------------------------------------------------------
{
  const arr1k = push_n(1000);
  bench('slice 1k', () => arr1k.slice(), 100);
  bench('concat 1k', () => arr1k.concat(arr1k), 50);
  bench('spread 1k', () => [...arr1k], 100);
}

// -------------------------------------------------------
// 8. Sort — reads + writes every element
// -------------------------------------------------------
{
  function make_random_arr(n) {
    const arr = [];
    let s = 42;
    for (let i = 0; i < n; i++) {
      s = (s * 1103515245 + 12345) & 0x7fffffff;
      arr.push(s % 10000);
    }
    return arr;
  }
  bench('sort 500', () => make_random_arr(500).sort((a, b) => a - b), 20);
}

// -------------------------------------------------------
// 9. Reverse
// -------------------------------------------------------
{
  const arr = push_n(1000);
  bench('reverse 1k', () => {
    arr.reverse();
  }, 100);
}

// -------------------------------------------------------
// 10. Array.from / entries / keys / values
// -------------------------------------------------------
{
  const arr = push_n(500);
  bench('Array.from 500', () => Array.from(arr), 50);
  bench('entries 500', () => arr.entries(), 50);
  bench('keys 500', () => arr.keys(), 50);
  bench('values 500', () => arr.values(), 50);
}

// -------------------------------------------------------
// 11. JSON.stringify — must iterate all elements
// -------------------------------------------------------
{
  const arr = push_n(500);
  bench('JSON.stringify 500', () => JSON.stringify(arr), 50);
}

// -------------------------------------------------------
// 12. Scaling test — show O(n²) behavior
// -------------------------------------------------------
console.log('\n--- scaling (push N, expect ~linear with dense) ---');
for (const n of [500, 1000, 2000, 4000]) {
  const t0 = now();
  push_n(n);
  const dt = now() - t0;
  console.log(`push ${n}: ${dt.toFixed(2)} ms`);
}

console.log('\n--- scaling (sequential read N, expect ~linear with dense) ---');
for (const n of [500, 1000, 2000, 4000]) {
  const arr = push_n(n);
  const t0 = now();
  index_read(arr, n);
  const dt = now() - t0;
  console.log(`read ${n}: ${dt.toFixed(2)} ms`);
}
