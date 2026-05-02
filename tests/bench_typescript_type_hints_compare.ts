const now = () => (typeof performance !== "undefined" && performance.now ? performance.now() : Date.now());

function readRounds(): number {
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw | 0 : 5000000;
}

function hot(
  a: number, b: number, c: number, d: number,
  e: number, f: number, g: number, h: number
): number {
  return (
    a + b + c + d + e + f + g + h +
    a + c + e + g + b + d + f + h +
    a + d + g + b + e + h + c + f +
    h + g + f + e + d + c + b + a
  );
}

function run(rounds: number): number {
  let total = 0;
  for (let i = 0; i < rounds; i++) {
    const n = i & 1023;
    total += hot(n, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7);
  }
  return total;
}

const rounds = readRounds();
run(2000);

let best = Infinity;
let checksum = 0;
for (let i = 0; i < 5; i++) {
  const t0 = now();
  checksum = run(rounds);
  const elapsed = now() - t0;
  if (elapsed < best) best = elapsed;
}

console.log("kind=ts");
console.log("rounds=" + rounds);
console.log("best_ms=" + best.toFixed(3));
console.log("checksum=" + checksum);
