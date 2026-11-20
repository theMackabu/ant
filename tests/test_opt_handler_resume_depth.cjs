const assert = require('node:assert');

const N = 30000;

function finallyRethrow(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    let local = i;
    try { try { throw local; } finally { local++; } } catch (e) { sum += local - e; }
  }
  return sum;
}

function nestedFinally(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { try { throw i; } finally { sum++; } } finally { sum++; }
    } catch (e) { sum += e === i ? 1 : 0; }
  }
  return sum;
}

function thrower(i) { throw i; }

function calleeThrows(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { thrower(i); } finally { sum++; } } catch (e) { sum += e === i ? 1 : 0; }
  }
  return sum;
}

function rareThrow(n) {
  let sum = 0;
  for (let i = 0; i < n * 16; i++) {
    try { try { if ((i & 15) === 0) throw i; sum++; } finally { sum++; } } catch (e) { sum += e === i ? 2 : 0; }
  }
  return sum;
}

function errorObject(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { null.x; } finally { sum++; } } catch (e) { sum += e instanceof TypeError ? 1 : 0; }
  }
  return sum;
}

function whileBreak(n) {
  let sum = 0, i = 0;
  while (true) {
    try { try { if (i++ >= n) break; throw i; } finally { sum++; } } catch (e) { sum += e === i ? 1 : 0; }
  }
  return sum;
}

function finallyContinue(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { throw i; } finally { sum++; continue; }
  }
  return sum;
}

function catchInFinally(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { sum++; } finally { try { throw i; } catch (e) { sum += e === i ? 1 : 0; } }
  }
  return sum;
}

function catchRethrow(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { throw i; } catch (e) { throw e + 1; } } catch (e) { sum += e - i; }
  }
  return sum;
}

function captures(n) {
  let sum = 0;
  const kept = [];
  for (let i = 0; i < n; i++) {
    let local = i;
    try {
      try { throw local; } finally { local += 2; if ((i & 8191) === 0) kept.push(() => local++); }
    } catch (e) { sum += local - e; }
  }
  for (const f of kept) sum += f() + f();
  return sum;
}

function usesArguments(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { throw i; } finally { sum += arguments.length; } } catch (e) { sum += arguments[1] + (e === i ? 1 : 0); }
  }
  return sum;
}

class Base { constructor() { this.base = 1; } }
class Derived extends Base {
  constructor(n) {
    super();
    this.sum = 0;
    for (let i = 0; i < n; i++) {
      try { try { throw i; } finally { this.sum += this.base; } } catch (e) { this.sum += e === i ? 1 : 0; }
    }
  }
}

function* generator(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { throw i; } finally { sum++; } } catch (e) { sum += e === i ? 1 : 0; }
    if ((i & 8191) === 0) yield sum;
  }
  return sum;
}

async function asyncLoop(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { try { throw i; } finally { sum++; } } catch (e) { sum += e === i ? 1 : 0; }
    if ((i & 8191) === 0) await null;
  }
  return sum;
}

function small(i) {
  let local = i;
  try { try { throw local; } finally { local++; } } catch (e) { return local - e; }
  return 0;
}

function check(name, run, expected) {
  let actual;
  try {
    actual = run();
  } catch (e) {
    assert.fail(`${name}: threw ${e && e.name}: ${e && e.message}`);
  }
  assert.strictEqual(actual, expected, name);
}

const cases = [
  ['finallyRethrow', finallyRethrow, N],
  ['nestedFinally', nestedFinally, 3 * N],
  ['calleeThrows', calleeThrows, 2 * N],
  ['rareThrow', rareThrow, N * 16 * 2 - N + 2 * N],
  ['errorObject', errorObject, 2 * N],
  ['whileBreak', whileBreak, 2 * N + 1],
  ['finallyContinue', finallyContinue, N],
  ['catchInFinally', catchInFinally, 2 * N],
  ['catchRethrow', catchRethrow, N],
];

for (const [name, fn, expected] of cases) {
  check(`${name} first call`, () => fn(N), expected);
  check(`${name} compiled call`, () => fn(N), expected);
}

const keptCount = Math.ceil(N / 8192);
let keptSum = 0;
for (let k = 0; k < keptCount; k++) keptSum += 2 * (k * 8192 + 2) + 1;
check('captures', () => captures(N), 2 * N + keptSum);
check('captures compiled call', () => captures(N), 2 * N + keptSum);
check('usesArguments', () => usesArguments(N, 3, 0), 3 * N + 3 * N + N);
check('usesArguments compiled call', () => usesArguments(N, 3, 0), 3 * N + 3 * N + N);
check('derived constructor', () => new Derived(N).sum, 2 * N);
check('derived constructor compiled call', () => new Derived(N).sum, 2 * N);

check('generator', () => {
  const it = generator(N);
  let step = it.next(), yields = 0;
  while (!step.done) { yields++; step = it.next(); }
  return step.value + yields;
}, 2 * N + keptCount);

let total = 0;
for (let i = 0; i < 4 * N; i++) total += small(i);
assert.strictEqual(total, 4 * N, 'inlined into a hot loop');

function manyCalls(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += small(i);
  return sum;
}
check('inlined callee', () => manyCalls(4 * N), 4 * N);

asyncLoop(N).then((sum) => {
  assert.strictEqual(sum, 2 * N, 'async loop');
  console.log('ok');
}, (e) => {
  console.error(e);
  process.exitCode = 1;
});
