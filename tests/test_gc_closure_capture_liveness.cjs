const assert = require('node:assert');

function wrap(f) {
  return function outer(y) {
    return f(y) * 2;
  };
}
function make(i) {
  return wrap(function inner(x) {
    return x + i;
  });
}

function churn(rounds) {
  let junk = [];
  for (let r = 0; r < rounds; r++) {
    for (let k = 0; k < 2000; k++) junk.push(make(k), { a: k, b: [k] });
    junk = [];
  }
}

const wrapped = [];
for (let i = 0; i < 2000; i++) wrapped.push(make(i));
churn(40);
let sum = 0;
for (const fn of wrapped) sum += fn(1);
assert.strictEqual(sum, 4002000);

function scope(seed) {
  function thrower(i) {
    if (i % 100 === 0) throw new Error(`boom ${i}`);
    return i + seed;
  }
  function catchEvery(n) {
    let s = 0, c = 0;
    for (let i = 0; i < n; i++) {
      try {
        s += thrower(i);
      } catch (e) {
        c++;
        s -= e.message.length;
      }
    }
    return [s, c];
  }
  return catchEvery;
}

const runners = [];
for (let i = 0; i < 50; i++) runners.push(scope(i));
const expected = runners.map((run, i) => run(1000)[0]);
for (let pass = 0; pass < 5; pass++) {
  churn(10);
  runners.forEach((run, i) => assert.deepStrictEqual(run(1000), [expected[i], 10]));
}

console.log('PASS closure capture liveness');
