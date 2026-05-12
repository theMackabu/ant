function normalize(pair) {
  if (pair[0] === 0) {
    pair[1] = 0;
    return pair;
  }

  while (pair[0] >= 10) {
    pair[0] /= 10;
    pair[1]++;
  }

  while (pair[0] < 1) {
    pair[0] *= 10;
    pair[1]--;
  }

  return pair;
}

function fib(n, a = new Float64Array([0, 0]), b = new Float64Array([1, 0])) {
  if (n === 0) return `${a[0].toPrecision(17)}e+${a[1]}`;

  const next = new Float64Array(2);

  if (a[1] > b[1]) {
    next[0] = a[0] + b[0] / 10 ** (a[1] - b[1]);
    next[1] = a[1];
  } else {
    next[0] = b[0] + a[0] / 10 ** (b[1] - a[1]);
    next[1] = b[1];
  }

  return fib(n - 1, b, normalize(next));
}

const result = fib(5000);
if (typeof result !== 'string' || !result.includes('e+')) {
  throw new Error('unexpected fibonacci result: ' + result);
}

console.log('Float64Array TCO GC regression ok');
