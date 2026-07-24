import { importedValue } from "./bench_jit_import_named_source.mjs";

const iterations = Number(process.argv[2] || 5_000_000);
const rounds = 7;

function run() {
  let sum = 0;
  for (let i = 0; i < iterations; i++) sum += importedValue;
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
console.log("named import JIT benchmark: " + iterations + " iterations x " + rounds + " rounds");
console.log(
  "live binding read: median=" + sorted[3].toFixed(2) + "ms samples=" +
  samples.map((value) => value.toFixed(2)).join(",") + " result=" + result,
);
