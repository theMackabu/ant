function normalize(mantissa, exponent) {
  if (mantissa === 0) return [0, 0];

  while (mantissa >= 10) {
    mantissa /= 10;
    exponent++;
  }

  while (mantissa < 1) {
    mantissa *= 10;
    exponent--;
  }

  return [mantissa, exponent];
}

function fib(n, am = 0, ae = 0, bm = 1, be = 0) {
  if (n === 0) return `${am.toPrecision(17)}e+${ae}`;

  let nm;
  let ne;

  if (ae > be) {
    nm = am + bm / 10 ** (ae - be);
    ne = ae;
  } else {
    nm = bm + am / 10 ** (be - ae);
    ne = be;
  }

  const next = normalize(nm, ne);
  return fib(n - 1, bm, be, next[0], next[1]);
}

const start = performance.now();
const n = 5000;
const result = fib(n);
const end = performance.now();

console.log(`fibonacci(${n}) ~= ${result}`);
console.log(`Time: ${(end - start).toFixed(4)} ms (${((end - start) * 1000).toFixed(2)} µs)`);
