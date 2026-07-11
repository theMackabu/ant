const iterations = Number(process.argv[2] || 1_000_000);
const rounds = Number(process.argv[3] || 7);

function FunctionContext(i) {
  this.request = i;
  this.store = i + 1;
  this.set = i + 2;
  this.path = i + 3;
  this.qi = i + 4;
  this.error = i + 5;
  this.redirect = i + 6;
  this.status = i + 7;
}

class BaseContext {}
class ClassContext extends BaseContext {
  constructor(i) {
    super();
    this.request = i;
    this.set = i + 7;
  }
}

function runFunction(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const context = new FunctionContext(i);
    sum += context.request + context.status;
  }
  return sum;
}

function runClass(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const context = new ClassContext(i);
    sum += context.request + context.set;
  }
  return sum;
}

function median(values) {
  const sorted = [];
  for (const value of values) {
    let index = sorted.length;
    while (index > 0 && sorted[index - 1] > value) index--;
    sorted.splice(index, 0, value);
  }
  return sorted[Math.floor(sorted.length / 2)];
}

function bench(name, fn, expected) {
  for (let i = 0; i < 150; i++) {
    if (fn(100) !== expected(100)) throw new Error(name + " warmup mismatch");
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    const result = fn(iterations);
    const elapsed = performance.now() - start;
    if (result !== expected(iterations)) throw new Error(name + " result mismatch");
    samples.push(elapsed);
  }

  console.log(
    name + ": median=" + median(samples).toFixed(2) +
    "ms samples=" + samples.map(value => value.toFixed(2)).join(",")
  );
}

console.log("constructor shape benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("function constructor (8 fields)", runFunction, n => n * n + 6 * n);
bench("derived class (2 fields)", runClass, n => n * n + 6 * n);
