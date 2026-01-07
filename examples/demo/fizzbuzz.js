function fizzbuzz(n) {
  const result = [];
  for (let i = 1; i <= n; i++) {
    if (i % 15 === 0) result.push('FizzBuzz');
    else if (i % 3 === 0) result.push('Fizz');
    else if (i % 5 === 0) result.push('Buzz');
    else result.push(i);
  }
  return result;
}

function fizzbuzz_bitwise(n) {
  const result = new Array(n);
  const F = 'Fizz',
    B = 'Buzz',
    FB = 'FizzBuzz';

  const mask = new Uint8Array([0, 0, 1, 0, 2, 1, 0, 0, 1, 2, 0, 1, 0, 0, 3]);

  for (let i = 0; i < n; ) {
    const batch = Math.min(15, n - i);
    for (let j = 0; j < batch; j++, i++) {
      const m = mask[i % 15];
      result[i] = m === 0 ? i + 1 : m === 1 ? F : m === 2 ? B : FB;
    }
  }
  return result;
}

function fizzbuzz_unrolled(n) {
  const r = new Array(n);
  const F = 'Fizz',
    B = 'Buzz',
    FB = 'FizzBuzz';
  let i = 0,
    num = 1;

  while (i + 15 <= n) {
    r[i++] = num++;
    r[i++] = num++;
    r[i++] = F;
    num++;
    r[i++] = num++;
    r[i++] = B;
    num++;
    r[i++] = F;
    num++;
    r[i++] = num++;
    r[i++] = num++;
    r[i++] = F;
    num++;
    r[i++] = B;
    num++;
    r[i++] = num++;
    r[i++] = F;
    num++;
    r[i++] = num++;
    r[i++] = num++;
    r[i++] = FB;
    num++;
  }

  const rem = [0, 0, 1, 0, 2, 1, 0, 0, 1, 2, 0, 1, 0, 0];
  const w = [0, F, B];
  while (i < n) {
    const m = rem[i % 15];
    r[i++] = m ? w[m] : num;
    num++;
  }

  return r;
}

function benchmark(name, fn, n, iterations = 100) {
  for (let i = 0; i < 10; i++) fn(n);

  const start = performance.now();
  for (let i = 0; i < iterations; i++) fn(n);
  const end = performance.now();

  const avg = (end - start) / iterations;
  console.log(`${name}: ${avg.toFixed(3)}ms for n=${n}`);
  return avg;
}

const N = 1_000;
console.log(`\nbenchmarking FizzBuzz implementations (n=${N.toLocaleString()}):\n`);

benchmark('normal', fizzbuzz, N);
benchmark('bitwise', fizzbuzz_bitwise, N);
benchmark('unrolled', fizzbuzz_unrolled, N);

function verify(fn) {
  const r = fn(15);
  const expected = [1, 2, 'Fizz', 4, 'Buzz', 'Fizz', 7, 8, 'Fizz', 'Buzz', 11, 'Fizz', 13, 14, 'FizzBuzz'];
  return JSON.stringify(r) === JSON.stringify(expected);
}

console.log('\ncorrectness check:');
console.log('normal:', verify(fizzbuzz));
console.log('bitwise:', verify(fizzbuzz_bitwise));
console.log('ultimate:', verify(fizzbuzz_unrolled));
