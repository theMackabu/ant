function nowMs() {
  if (typeof performance !== "undefined" && performance && typeof performance.now === "function")
    return performance.now();
  return Date.now();
}

function parseScale() {
  if (typeof process === "undefined" || !process || !process.argv) return 1;
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw : 1;
}

function sortNumbers(values) {
  const out = values.slice();
  for (let i = 1; i < out.length; i++) {
    const v = out[i];
    let j = i - 1;
    while (j >= 0 && out[j] > v) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = v;
  }
  return out;
}

function median(values) {
  const sorted = sortNumbers(values);
  return sorted[(sorted.length / 2) | 0];
}

const SCALE = parseScale();
const RUNS = 7;
let sink = 0;

function bench(label, iterations, fn) {
  for (let i = 0; i < 3; i++) sink ^= fn(Math.max(1, (iterations / 10) | 0)) | 0;

  const samples = [];
  let result = 0;
  for (let i = 0; i < RUNS; i++) {
    const t0 = nowMs();
    result = fn(iterations);
    samples.push(nowMs() - t0);
  }

  sink ^= result | 0;
  const med = median(samples);
  const opsPerMs = med > 0 ? (iterations / med).toFixed(2) : "inf";
  console.log(label + ": " + med.toFixed(3) + "ms, " + opsPerMs + " ops/ms, result=" + result);
  return med;
}

function Sequence(start) {
  this.item = start;
}

Sequence.prototype.next = function() {
  const old = this.item;
  this.item = old + 2;
  return old;
};

function runInlineableMethod(n) {
  const seq = new Sequence(1);
  let out = 0;
  for (let i = 0; i < n; i++) out = seq.next();
  return out + seq.item;
}

function runManualEquivalent(n) {
  const seq = new Sequence(1);
  let out = 0;
  for (let i = 0; i < n; i++) {
    out = seq.item;
    seq.item = out + 2;
  }
  return out + seq.item;
}

function PolyA(start) {
  this.item = start;
}

function PolyB(start) {
  this.padding = 1;
  this.item = start;
}

PolyA.prototype.next = Sequence.prototype.next;
PolyB.prototype.next = Sequence.prototype.next;

function callPoly(obj) {
  return obj.next();
}

function runPolymorphicMethod(n) {
  const a = new PolyA(1);
  const b = new PolyB(1);
  let out = 0;
  for (let i = 0; i < n; i++) out = callPoly((i & 1) === 0 ? a : b);
  return out + a.item + b.item;
}

function Vec2(x, y) {
  this.x = x;
  this.y = y;
}

Vec2.prototype.sumAndBump = function(dx, dy) {
  const old = this.x + this.y;
  this.x = this.x + dx;
  this.y = this.y + dy;
  return old;
};

function runInlineableArgs(n) {
  const v = new Vec2(1, 2);
  let out = 0;
  for (let i = 0; i < n; i++) out = v.sumAndBump(1, 2);
  return out + v.x + v.y;
}

function runManualArgs(n) {
  const v = new Vec2(1, 2);
  let out = 0;
  for (let i = 0; i < n; i++) {
    out = v.x + v.y;
    v.x = v.x + 1;
    v.y = v.y + 2;
  }
  return out + v.x + v.y;
}

function LargeMethod(start) {
  this.item = start;
  this.flag = false;
}

LargeMethod.prototype.next = function() {
  const old = this.item;
  this.item = old + 2;
  if (this.flag) {
    let sum = 0;
    sum = sum + old;
    sum = sum + this.item;
    sum = sum + old;
    sum = sum + this.item;
    return sum;
  }
  return old;
};

function runOverBudgetMethod(n) {
  const seq = new LargeMethod(1);
  let out = 0;
  for (let i = 0; i < n; i++) out = seq.next();
  return out + seq.item;
}

const iterations = Math.max(100000, Math.floor(1000000 * SCALE));

console.log("method inlining benchmark");
console.log("scale: " + SCALE);
console.log("iterations: " + iterations);

const manual = bench("manual slot loop", iterations, runManualEquivalent);
const inlineable = bench("inlineable method next()", iterations, runInlineableMethod);
const poly = bench("polymorphic method fallback", iterations, runPolymorphicMethod);
const manualArgs = bench("manual arg slot loop", iterations, runManualArgs);
const inlineArgs = bench("inlineable method with args", iterations, runInlineableArgs);
const overBudget = bench("over-budget method", iterations, runOverBudgetMethod);

console.log("next()/manual: " + (inlineable / manual).toFixed(2) + "x");
console.log("polymorphic/inlineable: " + (poly / inlineable).toFixed(2) + "x");
console.log("args/manual: " + (inlineArgs / manualArgs).toFixed(2) + "x");
console.log("over-budget/inlineable: " + (overBudget / inlineable).toFixed(2) + "x");
console.log("sink: " + sink);
