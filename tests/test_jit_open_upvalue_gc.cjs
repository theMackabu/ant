// A JIT frame keeps the upvalues its closures capture in a private open list
// whose head lives on the native stack. The collector reaches that list only
// through the conservative stack scan, and that walk used to stop at the
// first node already marked through a live closure, so nodes behind it were
// swept while still linked. The next capture reused a freed node and linked
// it into the list that still pointed at it, and closing the frame's
// upvalues then spun forever on the cycle.
//
// Shape: this module is OSR-compiled at its first loop, so the closures below
// are created by JIT code. The first two capture low slots and die; the third
// captures a higher slot (the list head) and allocates enough to collect
// while it is alive; the fourth captures a new, higher slot and gets one of
// the swept nodes back.
const dense = [];
for (let i = 0; i < 2048; i++) dense.push(i);
const a = { v: 1 }, b = { v: 2 }, c = { v: 3 }, d = { v: 4 };
function run(n, fn) { let r = 0; for (let k = 0; k < n; k++) r += fn(); return r; }
let total = 0;
total += run(1000, () => new Array(16).fill(a.v).length + a.v);
total += run(1000, () => new Array(16).fill(b.v).length + b.v);
total += run(300000, () => ({ x: c.v, y: [c.v] }).y.length + c.v);
total += run(1000, () => new Array(16).fill(d.v).length + d.v);
total += run(1000, () => new Array(16).fill(dense[7]).length + a.v + d.v);
if (total !== 1276000) throw new Error('checksum mismatch: ' + total);

// The same shape inside a function compiled on the call path, which is how
// it was reachable before large bodies could OSR at all.
function body(big) {
  const p = { v: 1 }, q = { v: 2 }, r = { v: 3 }, s = { v: 4 };
  let sum = 0;
  sum += run(10, () => new Array(16).fill(p.v).length + p.v);
  sum += run(10, () => new Array(16).fill(q.v).length + q.v);
  sum += run(big ? 300000 : 10, () => ({ x: r.v, y: [r.v] }).y.length + r.v);
  sum += run(10, () => new Array(16).fill(s.v).length + s.v);
  sum += run(10, () => new Array(16).fill(1).length + p.v + s.v);
  return sum;
}
for (let i = 0; i < 300; i++) body(false);
if (body(true) !== 1200760) throw new Error('call-path checksum mismatch');
console.log('jit-open-upvalue-gc: ok');
