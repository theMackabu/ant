const iterations = Number(process.argv[2] || 50_000);
const rounds = Number(process.argv[3] || 7);

const request = new Request("http://localhost/");
let sink;

class Base {}

class RequestContext extends Base {
  constructor(value) {
    super();
    this.request = value;
  }
}

class FullContext extends Base {
  constructor(value) {
    super();
    this.request = value;
    this.set = {
      headers: Object.create(null),
      status: undefined,
      cookie: undefined,
    };
  }
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
}

function bench(name, fn) {
  for (let i = 0; i < 2_000; i++) sink = fn();

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    for (let i = 0; i < iterations; i++) sink = fn();
    samples.push(performance.now() - start);
  }

  console.log(
    name + ": median=" + median(samples).toFixed(2) + "ms samples=" +
    samples.map((value) => value.toFixed(2)).join(","),
  );
}

console.log("context construction benchmark: " + iterations + " iterations x " + rounds + " rounds");
bench("plain object", () => ({}));
bench("null-prototype object", () => Object.create(null));
bench("set object", () => ({
  headers: Object.create(null),
  status: undefined,
  cookie: undefined,
}));
bench("derived context receiver", () => new RequestContext(request));
bench("full context", () => new FullContext(request));

if (sink === undefined) throw new Error("benchmark result was not observed");
