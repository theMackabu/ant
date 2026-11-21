import assert from 'node:assert';

function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) {
    const o = { i, s: 'x' + i, a: [i] };
    if ((i & 4095) === 0) keep = o;
  }
  return keep;
}

const N = 4000;
let good = 0, bad = 0;
const count = passed => { if (passed) good++; else bad++; };

function flood(from) {
  for (let i = from; i < from + N; i++) {
    const tag = 'v' + i;
    switch (i % 4) {
      case 0:
        import('node:path').then(m => count(typeof m.join === 'function' && tag === 'v' + i));
        break;
      case 1:
        import('data:text/javascript,export default ' + i + '; export const tag = "v' + i + '"')
          .then(m => count(m.default === i && m.tag === tag));
        break;
      case 2:
        import('data:text/javascript,export default ' + JSON.stringify({ i, tag }), { with: {} })
          .then(m => count(m.default.i === i && m.default.tag === tag));
        break;
      case 3:
        import('./missing-import-job-' + i + '.mjs').then(() => count(false), error => count(error instanceof Error && tag === 'v' + i));
        break;
    }
    if (i % 64 === 0) churn(25000);
  }
  churn(200000);
}

flood(0);
await new Promise(resolve => setTimeout(resolve, 1));
flood(N);
queueMicrotask(() => { flood(2 * N); churn(100000); });

for (let polls = 0; good + bad < 3 * N && polls < 1000; polls++) await new Promise(resolve => setTimeout(resolve, 10));
churn(200000);
assert.strictEqual(bad, 0);
assert.strictEqual(good, 3 * N);
console.log('ok');
