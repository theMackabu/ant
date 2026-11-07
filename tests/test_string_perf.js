const N = 100_000_000;
const buf = new Array(1024).fill(null);
const parts = Array.from({ length: 16 }, (_, k) => String.fromCharCode(97 + k));

function loop_only(n) { for (let i = 0; i < n; i++) buf[i & 1023] = null; }
function literal(n) { for (let i = 0; i < n; i++) buf[i & 1023] = ""; }
function ctor(n) { for (let i = 0; i < n; i++) buf[i & 1023] = String(); }
function from_int(n) { for (let i = 0; i < n; i++) buf[i & 1023] = String(i); }
function concat(n) { for (let i = 0; i < n; i++) buf[i & 1023] = parts[i & 15] + "x"; }

const cases = [["loop only", loop_only], ['""', literal], ["String()", ctor],
  ["String(i)", from_int], ["p[i&15] + 'x'", concat]];

const rt = typeof Ant !== "undefined" ? `Ant ${Ant.version}` : `Node ${process.version}`;
console.log(rt);
for (const [name, f] of cases) {
  f(1_000_000); // warm up
  let best = Infinity;
  for (let r = 0; r < 5; r++) {
    const t0 = process.hrtime.bigint();
    f(N);
    const dt = Number(process.hrtime.bigint() - t0);
    if (dt < best) best = dt;
  }
  const ns = best / N;
  console.log(`${name.padEnd(16)} ${ns.toFixed(2).padStart(7)} ns/string  ${(1e3 / ns).toFixed(1).padStart(8)} M/s`);
}
