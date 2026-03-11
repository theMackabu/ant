const now = () => (typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now());

function readScale() {
  if (typeof process === 'undefined' || !process || !process.argv) return 1;
  const raw = Number(process.argv[2]);
  return Number.isFinite(raw) && raw > 0 ? raw : 1;
}

const SCALE = readScale();
const REPEATS = 5;

function opaque(v) {
  return v;
}

function runCase(name, rounds, fn) {
  // Warm up once with a smaller trip count.
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
  console.log(`${name}: best ${best.toFixed(2)} ms, avg ${avg.toFixed(2)} ms`);
  return { best, avg, out };
}

function reportPair(title, rounds, typedFn, unknownFn) {
  console.log(`\n== ${title} (${rounds} rounds) ==`);
  const typed = runCase('typed-friendly', rounds, typedFn);
  const unknown = runCase('type-unknown ', rounds, unknownFn);

  if (typed.out !== unknown.out) {
    throw new Error(`${title}: result mismatch (${typed.out} vs ${unknown.out})`);
  }

  const ratio = unknown.best / typed.best;
  const delta = unknown.best - typed.best;
  const winner = ratio >= 1 ? 'typed-friendly' : 'type-unknown';
  console.log(`speedup: ${ratio.toFixed(2)}x (winner: ${winner}, delta ${delta.toFixed(2)} ms)`);
  return typed.out;
}

function forOfArrayTyped(rounds) {
  const arr = [
    1, 3, 5, 7, 9, 11, 13, 15,
    17, 19, 21, 23, 25, 27, 29, 31,
    33, 35, 37, 39, 41, 43, 45, 47,
    49, 51, 53, 55, 57, 59, 61, 63
  ];
  let sum = 0;
  for (let r = 0; r < rounds; r++) {
    for (const v of arr) sum += v;
  }
  return sum;
}

function forOfArrayUnknown(rounds) {
  const arr = [
    1, 3, 5, 7, 9, 11, 13, 15,
    17, 19, 21, 23, 25, 27, 29, 31,
    33, 35, 37, 39, 41, 43, 45, 47,
    49, 51, 53, 55, 57, 59, 61, 63
  ];
  const iterSrc = opaque(arr);
  let sum = 0;
  for (let r = 0; r < rounds; r++) {
    for (const v of iterSrc) sum += v;
  }
  return sum;
}

function forOfStringTyped(rounds) {
  const str = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  let sum = 0;
  for (let r = 0; r < rounds; r++) {
    for (const ch of str) sum += ch.length;
  }
  return sum;
}

function forOfStringUnknown(rounds) {
  const str = opaque('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789');
  let sum = 0;
  for (let r = 0; r < rounds; r++) {
    for (const ch of str) sum += ch.length;
  }
  return sum;
}

function arithmeticTyped(rounds) {
  const a = 1.25;
  const b = 3.75;
  const c = 0.5;
  const d = 9.125;
  let acc = 0;
  for (let i = 0; i < rounds; i++) {
    acc = acc + (a * b) - (c / d) + (a + c) - (b - d);
  }
  return acc;
}

function arithmeticUnknown(rounds) {
  const a = opaque(1.25);
  const b = opaque(3.75);
  const c = opaque(0.5);
  const d = opaque(9.125);
  let acc = 0;
  for (let i = 0; i < rounds; i++) {
    acc = acc + (a * b) - (c / d) + (a + c) - (b - d);
  }
  return acc;
}

function typeofGuardTyped(rounds) {
  const x = 123;
  let sum = 0;
  for (let i = 0; i < rounds; i++) {
    if (typeof x === 'number') sum += x;
    else sum -= 1;
  }
  return sum;
}

function typeofGuardUnknown(rounds) {
  const x = opaque(123);
  let sum = 0;
  for (let i = 0; i < rounds; i++) {
    if (typeof x === 'number') sum += x;
    else sum -= 1;
  }
  return sum;
}

const roundsForOfArray = Math.max(20000, Math.floor(70000 * SCALE));
const roundsForOfString = Math.max(15000, Math.floor(50000 * SCALE));
const roundsArithmetic = Math.max(500000, Math.floor(2500000 * SCALE));
const roundsTypeof = Math.max(500000, Math.floor(3000000 * SCALE));

console.log('compile-time type tracking benchmark');
console.log(`scale: ${SCALE}`);
console.log(`repeats: ${REPEATS}`);
console.log('usage: ./build/ant tests/bench_type_tracking.js [scale]');

let checksum = 0;
checksum += reportPair('for..of array iterator hint', roundsForOfArray, forOfArrayTyped, forOfArrayUnknown);
checksum += reportPair('for..of string iterator hint', roundsForOfString, forOfStringTyped, forOfStringUnknown);
checksum += reportPair('numeric arithmetic specialization', roundsArithmetic, arithmeticTyped, arithmeticUnknown);
checksum += reportPair('typeof guard folding', roundsTypeof, typeofGuardTyped, typeofGuardUnknown);

console.log(`\nchecksum: ${checksum}`);
