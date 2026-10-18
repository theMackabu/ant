const assert = require('node:assert');

const warm = (f, ...a) => { let r; for (let k = 0; k < 8; k++) r = f(...a); return r; };
function dummy() {}

function afterCall(n) { dummy(); let s = 0; for (let k = 0; k < n; k++) s += k * 2; return s; }
assert.strictEqual(warm(afterCall, 20000), 399980000);

function tdz(n, early) {
  let r = 'none';
  try { if (early) r = s; } catch (e) { r = e.name; }
  let s = 0;
  for (let k = 0; k < n; k++) s += k;
  return r + ':' + s;
}
assert.strictEqual(warm(tdz, 20000, false), 'none:199990000');
assert.strictEqual(tdz(20000, true), 'ReferenceError:199990000');
assert.strictEqual(warm(tdz, 20000, true), 'ReferenceError:199990000');

function reused(n) {
  let acc = 0;
  const seen = [];
  for (let k = 0; k < n; k++) {
    { let a = k * 1.5; acc += a; }
    { let b; if (k & 1) b = k; if (k < 4) seen.push(String(b)); }
  }
  return acc + '|' + seen.join();
}
assert.strictEqual(warm(reused, 20000), '299985000|undefined,1,undefined,3');

function bailBefore(arr, n) {
  let t = 0;
  for (let k = 0; k < n; k++) t += arr[k % arr.length];
  dummy();
  let s = 0;
  for (let k = 0; k < n; k++) s += k;
  return t + ':' + s;
}
assert.strictEqual(warm(bailBefore, [1, 2, 3], 30000), '60000:449985000');
assert.strictEqual(bailBefore([1, 'x', 3], 6), '1x31x3:15');
assert.strictEqual(warm(bailBefore, [1, 2, 3], 30000), '60000:449985000');

function bodyLet(n) {
  const seen = [];
  let sum = 0;
  for (let k = 0; k < n; k++) {
    dummy();
    let t;
    if (k % 3 === 0) t = k * 0.5;
    if (k < 6) seen.push(String(t));
    if (t !== undefined) sum += t;
  }
  return sum + '|' + seen.join();
}
assert.strictEqual(warm(bodyLet, 30000), '74992500|0,undefined,undefined,1.5,undefined,undefined');

async function afterAwait(n) { await null; let s = 0; for (let k = 0; k < n; k++) s += k * 2; return s; }

(async () => {
  for (let k = 0; k < 8; k++) assert.strictEqual(await afterAwait(20000), 399980000);
  console.log('PASS late numeric let locals');
})();
