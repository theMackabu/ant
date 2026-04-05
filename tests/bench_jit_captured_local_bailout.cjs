const now = () => (typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now());

function readScale() {
  if (typeof process === 'undefined' || !process || !process.argv) return 1;
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw : 1;
}

const SCALE = readScale();
const REPEATS = 5;

function bench(name, rounds, fn) {
  fn(Math.max(1, (rounds / 8) | 0));

  const samples = [];
  let out = 0;
  for (let i = 0; i < REPEATS; i++) {
    const t0 = now();
    out = fn(rounds);
    samples.push(now() - t0);
  }

  let best = samples[0];
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    if (samples[i] < best) best = samples[i];
    sum += samples[i];
  }

  const avg = sum / samples.length;
  console.log(`${name}: best ${best.toFixed(2)} ms, avg ${avg.toFixed(2)} ms, out ${out}`);
  return out;
}

function hotCapturedLocalBailout(rounds) {
  function f(val, y) {
    let x = val + 1;
    const g = () => x;
    if (typeof y === 'number') return g();
    return 0;
  }

  let sum = 0;
  for (let i = 0; i < rounds; i++) sum += f(i, i);
  return sum;
}

const rounds = 8_000_000 * SCALE;
console.log(`jit captured-local bailout benchmark (${rounds} rounds)`);
bench('captured local + bailout', rounds, hotCapturedLocalBailout);
