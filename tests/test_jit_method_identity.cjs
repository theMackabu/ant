function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function make(k) {
  return function (x) { return x + this.v * k; };
}

function T(v) { this.v = v; }
T.prototype.m = make(1);

const o = new T(2);
function run(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = o.m(s) & 0xffff;
  return s;
}

for (let i = 0; i < 2000; i++) run(50);
assert(run(10) === 20, 'initial method');

T.prototype.m = make(3);
assert(run(10) === 60, 'same source, new closure');

T.prototype.m = function (x) { return x - this.v; };
assert(run(10) === ((-20) & 0xffff), 'replaced method');

const other = { v: 100 };
T.prototype.m = make(1).bind(other);
assert(run(10) === 1000, 'bound method keeps its this');

T.prototype.m = (x) => x + 7;
assert(run(10) === 70, 'arrow method');

function fresh() {
  return function (x) { return x + this.v * 5; };
}

function churn(rounds, width) {
  const ring = new Array(8);
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(width);
    for (let i = 0; i < width; i++) batch[i] = { i, text: 'x' + i, arr: [i, i + 1, i + 2, i + 3] };
    ring[r & 7] = batch;
  }
}

// after collections free the replaced method, new closures of another function
// may reuse its memory; the compiled caller must still call the current method
T.prototype.m = make(1);
for (let i = 0; i < 2000; i++) run(50);
T.prototype.m = make(2);
churn(400, 2000);
for (let i = 0; i < 20000; i++) {
  T.prototype.m = fresh();
  assert(run(1) === 10, 'stale inline body after gc at ' + i);
}

o.m = function (x) { return x + 1; };
assert(run(10) === 10, 'own property shadows prototype');

console.log('jit method identity OK');
