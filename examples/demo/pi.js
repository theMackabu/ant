const iterations = 1_000_000;

function calculatePiLeibniz() {
  let pi = 0;
  let denom = 1;

  const limit = iterations - (iterations % 4);
  for (let i = 0; i < limit; i += 4) {
    pi += 1 / denom - 1 / (denom + 2) + 1 / (denom + 4) - 1 / (denom + 6);
    denom += 8;
  }

  let sign = 1;
  for (let i = limit; i < iterations; i++) {
    pi += sign / denom;
    sign = -sign;
    denom += 2;
  }
  return pi * 4;
}

const start = performance.now();
const result = calculatePiLeibniz();
const end = performance.now();

console.log(`Pi (${iterations}) ≈ ${result}`);
console.log(`Time: ${(end - start).toFixed(2)} ms`);
