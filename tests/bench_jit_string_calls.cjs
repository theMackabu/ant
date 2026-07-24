const iterations = Number(process.argv[2] || 2_000_000);
const rounds = 7;
const url = "http://localhost/path?x=1";

function parsePath() {
  const start = url.indexOf("/", 16);
  const query = url.indexOf("?", start);
  return url.substring(start, query === -1 ? url.length : query);
}

function run() {
  let sum = 0;
  for (let i = 0; i < iterations; i++) sum += parsePath().length;
  return sum;
}

for (let i = 0; i < 2; i++) run();
const samples = [];
let result = 0;
for (let i = 0; i < rounds; i++) {
  const start = performance.now();
  result = run();
  samples.push(performance.now() - start);
}
const sorted = samples.slice().sort((a, b) => a - b);
console.log("string call JIT benchmark: " + iterations + " iterations x " + rounds + " rounds");
console.log(
  "URL path parse: median=" + sorted[3].toFixed(2) + "ms samples=" +
  samples.map((value) => value.toFixed(2)).join(",") + " result=" + result,
);
