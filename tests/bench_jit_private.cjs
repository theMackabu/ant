const iterations = Number(process.argv[2] || 2_000_000);
const rounds = Number(process.argv[3] || 7);

class PrivateBench {
  #value = 0;

  #addOne(value) {
    return value + 1;
  }

  get #accessor() {
    return this.#value;
  }

  set #accessor(value) {
    this.#value = value;
  }

  read(n) {
    this.#value = 7;
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.#value;
    return sum;
  }

  write(n) {
    for (let i = 0; i < n; i++) this.#value = i;
    return this.#value;
  }

  increment(n) {
    this.#value = 0;
    for (let i = 0; i < n; i++) this.#value++;
    return this.#value;
  }

  callMethod(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.#addOne(i);
    return sum;
  }

  accessorRoundtrip(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
      this.#accessor = i & 7;
      sum += this.#accessor;
    }
    return sum;
  }
}

class PublicBench {
  value = 0;

  addOne(value) {
    return value + 1;
  }

  get accessor() {
    return this.value;
  }

  set accessor(value) {
    this.value = value;
  }

  read(n) {
    this.value = 7;
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.value;
    return sum;
  }

  write(n) {
    for (let i = 0; i < n; i++) this.value = i;
    return this.value;
  }

  increment(n) {
    this.value = 0;
    for (let i = 0; i < n; i++) this.value++;
    return this.value;
  }

  callMethod(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.addOne(i);
    return sum;
  }

  accessorRoundtrip(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) {
      this.accessor = i & 7;
      sum += this.accessor;
    }
    return sum;
  }
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function cycleSum(n) {
  const remainder = n % 8;
  return Math.floor(n / 8) * 28 + remainder * (remainder - 1) / 2;
}

function bench(name, fn, expected) {
  for (let i = 0; i < 150; i++) {
    if (fn(100) !== expected(100)) throw new Error(name + " warmup mismatch");
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = Date.now();
    const result = fn(iterations);
    const elapsed = Date.now() - start;
    if (result !== expected(iterations)) throw new Error(name + " result mismatch");
    samples.push(elapsed);
  }

  console.log(name + ": median=" + median(samples) + "ms samples=" + samples.join(","));
}

const privateBench = new PrivateBench();
const publicBench = new PublicBench();

console.log("private JIT benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("public field read", n => publicBench.read(n), n => n * 7);
bench("private field read", n => privateBench.read(n), n => n * 7);
bench("public field write", n => publicBench.write(n), n => n - 1);
bench("private field write", n => privateBench.write(n), n => n - 1);
bench("public field increment", n => publicBench.increment(n), n => n);
bench("private field increment", n => privateBench.increment(n), n => n);
bench("public method call", n => publicBench.callMethod(n), n => n * (n + 1) / 2);
bench("private method call", n => privateBench.callMethod(n), n => n * (n + 1) / 2);
bench("public accessor roundtrip", n => publicBench.accessorRoundtrip(n), cycleSum);
bench("private accessor roundtrip", n => privateBench.accessorRoundtrip(n), cycleSum);
