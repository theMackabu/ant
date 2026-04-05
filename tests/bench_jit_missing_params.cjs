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

function hotMissingParamWrite(rounds) {
  function f(val, options) {
    options = options || {};
    if (typeof val === 'number') return options.long ? 2 : 3;
    return 0;
  }

  let sum = 0;
  for (let i = 0; i < rounds; i++) sum += f(i);
  return sum;
}

function hotPresentParamWrite(rounds) {
  function f(val, options) {
    options = options || {};
    if (typeof val === 'number') return options.long ? 2 : 3;
    return 0;
  }

  const shared = {};
  let sum = 0;
  for (let i = 0; i < rounds; i++) sum += f(i, shared);
  return sum;
}

function hotNoParamWrite(rounds) {
  function f(val, options) {
    if (!options) options = {};
    if (typeof val === 'number') return options.long ? 2 : 3;
    return 0;
  }

  const shared = {};
  let sum = 0;
  for (let i = 0; i < rounds; i++) sum += f(i, shared);
  return sum;
}

function hotLocalWrite(rounds) {
  function f(val, options) {
    let opts = options || {};
    if (typeof val === 'number') return opts.long ? 2 : 3;
    return 0;
  }

  let sum = 0;
  for (let i = 0; i < rounds; i++) sum += f(i);
  return sum;
}

const rounds = 8_000_000 * SCALE;
console.log(`jit missing-parameter benchmark (${rounds} rounds)`);
bench('missing param write', rounds, hotMissingParamWrite);
bench('present param write', rounds, hotPresentParamWrite);
bench('no param write', rounds, hotNoParamWrite);
bench('local alias write', rounds, hotLocalWrite);
