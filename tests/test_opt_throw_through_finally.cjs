const assert = require('node:assert');

const ROUNDS = 20000;

let top = 0, cleaned = 0;
for (let i = 0; i < ROUNDS; i++) {
  try { try { throw 1; } finally { cleaned++; } } catch (e) { top += e; }
}
assert.strictEqual(top, ROUNDS);
assert.strictEqual(cleaned, ROUNDS);

function nested(n) {
  let caught = 0, ran = 0;
  for (let i = 0; i < n; i++) {
    try { try { throw i; } finally { ran++; } } catch (e) { caught += e === i ? 1 : 0; }
  }
  return [caught, ran];
}
assert.deepStrictEqual(nested(ROUNDS), [ROUNDS, ROUNDS]);

function thrower(i) { if (i >= 0) throw new RangeError('r' + i); return i; }
function calleeThrough(n) {
  let seen = 0, ran = 0;
  for (let i = 0; i < n; i++) {
    try { try { thrower(i); } finally { ran++; } } catch (e) { if (e instanceof RangeError && e.message === 'r' + i) seen++; }
  }
  return [seen, ran];
}
assert.deepStrictEqual(calleeThrough(ROUNDS), [ROUNDS, ROUNDS]);

function finallyOnly(i) { try { throw i; } finally { finallyOnly.ran++; } }
finallyOnly.ran = 0;
function outer(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) { try { finallyOnly(i); } catch (e) { sum += e & 1; } }
  return sum;
}
assert.strictEqual(outer(ROUNDS), ROUNDS / 2);
assert.strictEqual(finallyOnly.ran, ROUNDS);

function rethrow(n) {
  let log = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } catch (e) { throw e + 1; } finally { log++; }
    } catch (e) { if (e === i + 1) log++; }
  }
  return log;
}
assert.strictEqual(rethrow(ROUNDS), 2 * ROUNDS);

function overridden(n) {
  const run = (i) => {
    try { try { throw i; } finally { if (i % 3 === 0) return 1; } } catch (e) { return 2; }
  };
  let sum = 0;
  for (let i = 0; i < n; i++) sum += run(i);
  return sum;
}
assert.strictEqual(overridden(3000), 1000 + 2 * 2000);

function sparseThrows(n) {
  let sum = 0, finals = 0;
  const seen = [];
  for (const base of [0, n]) {
    for (let i = 0; i < n; i++) {
      let step = i + base;
      const read = () => step;
      try {
        try {
          if (i % 700 === 0) throw step;
          sum += step & 3;
        } finally {
          step++;
          finals++;
        }
      } catch (e) {
        seen.push(e, read(), arguments.length);
      }
    }
  }
  return [sum, finals, seen.length, seen[0], seen[1], seen[2], seen[seen.length - 3], seen[seen.length - 2]].join();
}
{
  const n = 4200;
  let sum = 0;
  for (let i = 0; i < 2 * n; i++) if (i % n % 700 !== 0) sum += i & 3;
  const expected = [sum, 2 * n, 36, 0, 1, 1, n + 3500, n + 3501].join();
  for (let round = 0; round < 3; round++) assert.strictEqual(sparseThrows(n), expected);
}

function throwsWithPendingBreak(n) {
  let out = 0;
  for (let i = 0; i < n; i++) {
    outer: for (const v of [1, 2, 3]) {
      try {
        try {
          if (v === 2) break outer;
          try { if (i % 3 === 0) throw v; } finally { out += 1; }
          out += 10;
        } finally {
          out += 100;
        }
      } catch (e) {
        out += 1000 * e;
      }
    }
  }
  return out;
}
{
  const third = Math.ceil(ROUNDS / 3);
  assert.strictEqual(throwsWithPendingBreak(ROUNDS), third * 1201 + (ROUNDS - third) * 211);
}

class ThrowingBase { constructor() { this.base = 1; } }
class ThrowingDerived extends ThrowingBase {
  constructor(n) {
    super();
    let total = 0;
    for (let i = 0; i < n; i++) {
      try { try { if (i % 500 === 0) throw i; total += 1; } finally { total += 2; } } catch (e) { total += this.base; }
    }
    this.total = total;
  }
}
for (let round = 0; round < 3; round++) assert.strictEqual(new ThrowingDerived(ROUNDS).total, 3 * ROUNDS);

console.log('ok');
