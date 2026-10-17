const assert = require('node:assert');

const N = 3000;

function closuresInBody() {
  const fns = [];
  for (let i = 0; i < N; i++) if (i % 1000 === 0) fns.push(() => i);
  return fns.map(f => f());
}
assert.deepStrictEqual(closuresInBody(), [0, 1000, 2000]);

function evalInBody() {
  const fns = [];
  for (let i = 0; i < 3; i++) fns.push(eval('() => i'));
  return fns.map(f => f());
}
assert.deepStrictEqual(evalInBody(), [0, 1, 2]);

function plainLoop(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i % 7 === 0) continue;
    s += i;
    if (i > 2 * N) break;
  }
  return s;
}
let expected = 0;
for (let i = 0; i < N; i++) if (i % 7 !== 0) expected += i;
for (let r = 0; r < 5; r++) assert.strictEqual(plainLoop(N), expected);

globalThis.specValue = 1;
function copyThenAdd(n) {
  const y = specValue;
  let s = 0;
  for (let i = 0; i < n; i++) s += y;
  return s;
}
for (let r = 0; r < 5; r++) assert.strictEqual(copyThenAdd(N), N);
globalThis.specValue = 'a';
assert.strictEqual(copyThenAdd(3), '0aaa');
globalThis.specValue = 2;
assert.strictEqual(copyThenAdd(N), 2 * N);

function readBeforeInit(n) {
  let hits = 0;
  for (let i = 0; i < n; i++) hits += i & 1;
  try {
    hits += late;
  } catch (e) {
    assert.ok(e instanceof ReferenceError);
    hits = -hits;
  }
  let late = 0;
  return hits + late;
}
for (let r = 0; r < 3; r++) assert.strictEqual(readBeforeInit(N), -N / 2);

globalThis.hoistedA = 1;
function globalShapeChanges(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += hoistedA;
    if (i === n - 10) globalThis[`fresh${n}`] = i;
    if (i === n - 5) globalThis.hoistedA = 3;
  }
  return s;
}
assert.strictEqual(globalShapeChanges(N), (N - 4) + 3 * 4);
assert.strictEqual(globalShapeChanges(N + 1), 3 * (N + 1));

console.log('jit loop speculation: ok');
