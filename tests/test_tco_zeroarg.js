// Test: verify zero-arg tail call actually uses TCO by going deep enough to
// guarantee a stack overflow without it.

// Pure zero-arg self-recursion with no parameters at all
let n = 0;
function go() {
  n++;
  if (n >= 1000000) return 'ok';
  return go();
}
const r = go();
console.log('result:', r, 'n:', n);
if (r !== 'ok') { console.log('FAIL'); process.exit(1); }

// Zero-arg mutual recursion
let m = 0;
function a() { m++; if (m >= 1000000) return m; return b(); }
function b() { m++; if (m >= 1000000) return m; return a(); }
const r2 = a();
console.log('mutual result:', r2);
if (r2 !== 1000000) { console.log('FAIL'); process.exit(1); }

console.log('PASS');
