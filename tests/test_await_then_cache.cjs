const assert = require('node:assert');

function make(i) { return { v: i, w: 1 }; }
async function ret(i) { return make(i); }
async function awaitPlain(i) { return (await make(i)).v; }
async function awaitReturned(i) { return (await ret(i)).v; }
async function awaitArray(i) { return (await [i, 1])[0]; }

class Box {
  constructor(v) { this.v = v; }
}

async function warm(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    s += await awaitPlain(i);
    s += await awaitReturned(i);
    s += await awaitArray(i);
    s += (await new Box(i)).v;
    s += (await Promise.resolve(make(i))).v;
    s += (await new Promise(r => r(make(i)))).v;
  }
  return s;
}

async function main() {
  assert.strictEqual(await warm(2000), 6 * 1999 * 2000 / 2);

  let calls = 0;
  Box.prototype.then = function (resolve) { calls++; resolve('box-then'); };
  assert.strictEqual(await new Box(1), 'box-then');
  assert.strictEqual(await Promise.resolve(new Box(2)), 'box-then');
  assert.strictEqual(calls, 2);
  delete Box.prototype.then;
  assert.strictEqual((await new Box(3)).v, 3);

  const late = make(5);
  late.then = function (resolve) { resolve('own-then'); };
  assert.strictEqual(await late, 'own-then');
  assert.strictEqual((await make(6)).v, 6);

  const solo = { a: 1, b: 2 };
  delete solo.a;
  assert.strictEqual((await solo).b, 2);
  assert.strictEqual((await solo).b, 2);
  solo.then = function (resolve) { resolve('solo-then'); };
  assert.strictEqual(await solo, 'solo-then');

  const swapped = make(7);
  assert.strictEqual((await swapped).v, 7);
  Object.setPrototypeOf(swapped, { then(resolve) { resolve('proto-swap'); } });
  assert.strictEqual(await swapped, 'proto-swap');

  let getterCalls = 0;
  const viaGetter = Object.create({ get then() { getterCalls++; return undefined; } });
  assert.strictEqual(await viaGetter, viaGetter);
  assert.strictEqual(await viaGetter, viaGetter);
  assert.strictEqual(getterCalls, 2);

  let arrayCalls = 0;
  Array.prototype.then = function (resolve) { arrayCalls++; resolve('array-then'); };
  assert.strictEqual(await [1, 2], 'array-then');
  assert.strictEqual(await awaitArray(1), 'a');
  delete Array.prototype.then;
  assert.strictEqual((await [9])[0], 9);
  assert.ok(arrayCalls >= 2);

  assert.strictEqual(await warm(200), 6 * 199 * 200 / 2);

  let objectCalls = 0;
  Object.prototype.then = function (resolve) {
    objectCalls++;
    delete Object.prototype.then;
    resolve('object-then');
  };
  assert.strictEqual(await awaitReturned(1).then(v => v, e => e), undefined);
  assert.ok(objectCalls >= 1);
  assert.strictEqual(Object.prototype.then, undefined);
  assert.strictEqual((await make(8)).v, 8);

  Object.prototype.then = function (resolve) { resolve('sticky'); };
  let observed;
  try {
    observed = await make(9);
  } finally {
    delete Object.prototype.then;
  }
  assert.strictEqual(observed, 'sticky');
  assert.strictEqual((await make(10)).v, 10);

  console.log('await then cache ok');
}

main().catch(e => { console.error(e); process.exit(1); });
