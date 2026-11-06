const assert = require('node:assert');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

const failures = [];
let pending = 0;

function later(name, run) {
  pending++;
  run((check) => (...cbArgs) => {
    try {
      check(...cbArgs);
    } catch (err) {
      failures.push(`${name}: ${err.message}`);
    }
    pending--;
  });
}

function bytesOf(view) {
  return Array.from(new Uint8Array(view.buffer ?? view, view.byteOffset ?? 0, view.byteLength));
}

function assertFilled(bytes, name) {
  assert.ok(bytes.some((byte) => byte !== 0), `${name} should be filled`);
}

function assertZero(bytes, name) {
  assert.ok(bytes.every((byte) => byte === 0), `${name} should be untouched`);
}

function assertInRange(n, min, max) {
  assert.ok(Number.isSafeInteger(n), `${n} should be a safe integer`);
  assert.ok(n >= min && n < max, `${n} should be in [${min}, ${max})`);
}

const seen = new Set();
for (let i = 0; i < 1000; i++) {
  const n = crypto.randomInt(3);
  assertInRange(n, 0, 3);
  seen.add(n);
}
assert.strictEqual(seen.size, 3);

for (let i = 0; i < 200; i++) {
  assertInRange(crypto.randomInt(-10, -8), -10, -8);
  assertInRange(crypto.randomInt(5, 6), 5, 6);
  assertInRange(crypto.randomInt(5, undefined), 0, 5);
}

const maxSafe = Number.MAX_SAFE_INTEGER;
const maxRange = 2 ** 48 - 1;
assertInRange(crypto.randomInt(maxSafe - 5, maxSafe), maxSafe - 5, maxSafe);
assertInRange(crypto.randomInt(-maxSafe, -maxSafe + 5), -maxSafe, -maxSafe + 5);
assertInRange(crypto.randomInt(maxRange), 0, maxRange);
assertInRange(crypto.randomInt(1, maxRange + 1), 1, maxRange + 1);

const biasedRange = Math.floor((2 ** 48 * 2) / 3);
let lowerHalf = 0;
const biasSamples = 20000;
for (let i = 0; i < biasSamples; i++) {
  if (crypto.randomInt(biasedRange) < biasedRange / 2) lowerHalf++;
}
assert.ok(Math.abs(lowerHalf / biasSamples - 0.5) < 0.03, `lower half ratio ${lowerHalf / biasSamples}`);

const safeIntegerError = (name) => ({ name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: new RegExp(`"${name}" argument must be a safe integer`) });
for (const bad of ['1', NaN, Infinity, 1.5, null, {}, [], 1n, true, maxSafe + 1, -maxSafe - 1]) {
  assert.throws(() => crypto.randomInt(bad), safeIntegerError('max'));
  assert.throws(() => crypto.randomInt(bad, 100), safeIntegerError('min'));
  assert.throws(() => crypto.randomInt(0, bad), safeIntegerError('max'));
}
assert.throws(() => crypto.randomInt(), safeIntegerError('max'));

const outOfRange = (message) => ({ name: 'RangeError', code: 'ERR_OUT_OF_RANGE', message });
assert.throws(() => crypto.randomInt(0), outOfRange(/"max".*greater than the value of "min" \(0\)/));
assert.throws(() => crypto.randomInt(2, 1), outOfRange(/"max".*greater than the value of "min" \(2\)/));
assert.throws(() => crypto.randomInt(-0, 0), outOfRange(/greater than the value of "min" \(0\)/));
assert.throws(() => crypto.randomInt(-maxSafe, -maxSafe), outOfRange(/"min" \(-9007199254740991\)/));
assert.throws(() => crypto.randomInt(2 ** 48), outOfRange(/"max" is out of range. It must be <= 281474976710655/));
assert.throws(() => crypto.randomInt(0, 2 ** 48), outOfRange(/"max - min" is out of range/));
assert.throws(() => crypto.randomInt(-maxSafe, maxSafe), outOfRange(/"max - min" is out of range/));

for (const bad of ['x', 1, null, {}]) {
  assert.throws(() => crypto.randomInt(1, 2, bad), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"callback"/ });
}
assert.throws(() => crypto.randomInt('x', () => assert.fail('callback must not run')), safeIntegerError('max'));

assert.strictEqual(crypto.randomInt.length, 3);
assert.strictEqual(crypto.randomFill.length, 4);
assert.strictEqual(crypto.randomFillSync.length, 1);

const uint32 = new Uint32Array(8);
assert.strictEqual(crypto.randomFillSync(uint32, 2, 4), uint32);
assertZero(bytesOf(uint32).slice(0, 8), 'leading elements');
assertFilled(bytesOf(uint32).slice(8, 24), 'selected elements');
assertZero(bytesOf(uint32).slice(24), 'trailing elements');

const uint32Tail = new Uint32Array(8);
crypto.randomFillSync(uint32Tail, 4);
assertZero(bytesOf(uint32Tail).slice(0, 16), 'leading elements');
assertFilled(bytesOf(uint32Tail).slice(16), 'tail elements');

const backing = new Uint8Array(48);
crypto.randomFillSync(backing.subarray(16, 32));
assertZero(Array.from(backing.subarray(0, 16)), 'before subarray');
assertFilled(Array.from(backing.subarray(16, 32)), 'subarray');
assertZero(Array.from(backing.subarray(32)), 'after subarray');

const viewBacking = new ArrayBuffer(48);
const dataView = new DataView(viewBacking, 16, 16);
assert.strictEqual(crypto.randomFillSync(dataView, 8), dataView);
assertZero(Array.from(new Uint8Array(viewBacking, 0, 24)), 'before DataView range');
assertFilled(Array.from(new Uint8Array(viewBacking, 24, 8)), 'DataView range');
assertZero(Array.from(new Uint8Array(viewBacking, 32)), 'after DataView range');

for (const target of [new ArrayBuffer(32), new SharedArrayBuffer(32), Buffer.alloc(32)]) {
  assert.strictEqual(crypto.randomFillSync(target), target);
  assertFilled(bytesOf(target), target.constructor.name);
}

const zeroSized = new Uint8Array(8);
crypto.randomFillSync(zeroSized, 8);
crypto.randomFillSync(zeroSized, 3, 0);
assertZero(Array.from(zeroSized), 'zero size fill');

const fractional = new Uint8Array(32);
crypto.randomFillSync(fractional, 16.5);
assertZero(Array.from(fractional.subarray(0, 16)), 'fractional offset truncates');
assertFilled(Array.from(fractional.subarray(16)), 'after fractional offset');

function assertFillValidation(fill) {
  const buf = new Uint8Array(10);
  const uint32Buf = new Uint32Array(4);

  assert.throws(() => fill(buf, 'x'), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"offset" argument must be of type number/ });
  assert.throws(() => fill(buf, 0, 'x'), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"size" argument must be of type number/ });

  for (const bad of [NaN, -10, 11, 2 ** 32]) {
    assert.throws(() => fill(buf, bad), outOfRange(/"offset" is out of range. It must be >= 0 && <= 10/));
  }
  for (const bad of [NaN, -1, 2 ** 31, 2 ** 32]) {
    assert.throws(() => fill(buf, 1, bad), outOfRange(/"size" is out of range. It must be >= 0 && <= 2147483647/));
  }

  assert.throws(() => fill(buf, 1, 10), outOfRange(/"size \+ offset" is out of range. It must be <= 10. Received 11/));
  assert.throws(() => fill(uint32Buf, 5), outOfRange(/"offset" is out of range. It must be >= 0 && <= 16. Received 20/));
  assert.throws(() => fill(uint32Buf, 1, 4), outOfRange(/"size \+ offset" is out of range. It must be <= 16. Received 20/));

  for (const bad of [1, true, NaN, null, undefined, {}, [], 'x']) {
    assert.throws(() => fill(bad), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"buf" argument must be an instance of ArrayBuffer or ArrayBufferView/ });
  }
}

assertFillValidation((...fillArgs) => crypto.randomFillSync(...fillArgs));

const failUnlessThrown = () => assert.fail('callback must not run');
assertFillValidation((buf, ...rest) => crypto.randomFill(buf, ...rest, failUnlessThrown));

for (const bad of [1, true, NaN, null, undefined, {}, [], 'x']) {
  assert.throws(() => crypto.randomFill(new Uint8Array(4), 0, 4, bad), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"callback" argument must be of type function/ });
}
assert.throws(() => crypto.randomFill(new Uint8Array(4)), { code: 'ERR_INVALID_ARG_TYPE', message: /"callback"/ });
assert.throws(() => crypto.randomFill(new Uint8Array(4), undefined, failUnlessThrown), { code: 'ERR_INVALID_ARG_TYPE', message: /"offset"/ });

assert.match(process.versions.openssl, /^\d+\.\d+\.\d+/);

const distinct = new Set();
for (let i = 0; i < 1000; i++) distinct.add(crypto.randomInt(maxRange));
assert.strictEqual(distinct.size, 1000);

for (const randomBytes of [crypto.randomBytes, crypto.pseudoRandomBytes, crypto.prng, crypto.rng]) {
  assert.strictEqual(randomBytes, crypto.randomBytes);
}

for (const alias of ['pseudoRandomBytes', 'prng', 'rng']) {
  const descriptor = Object.getOwnPropertyDescriptor(crypto, alias);
  assert.strictEqual(descriptor.enumerable, false);
  assert.strictEqual(descriptor.configurable, true);
  assert.strictEqual(descriptor.writable, true);
}

for (const [size, expected] of [[0, 0], [1, 1], [101.2, 101], [70000, 70000]]) {
  const bytes = crypto.randomBytes(size);
  assert.ok(Buffer.isBuffer(bytes));
  assert.strictEqual(bytes.length, expected);
  if (expected > 16) assertFilled(Array.from(bytes), 'randomBytes result');
}

for (const bad of [undefined, null, false, true, {}, [], '1']) {
  assert.throws(() => crypto.randomBytes(bad), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"size" argument must be of type number/ });
}
for (const bad of [-1, NaN, 2 ** 31, 2 ** 32]) {
  assert.throws(() => crypto.randomBytes(bad), outOfRange(/"size" is out of range. It must be >= 0 && <= 2147483647/));
  assert.throws(() => crypto.randomBytes(bad, failUnlessThrown), outOfRange(/"size" is out of range/));
}
for (const bad of [1, true, NaN, null, {}, []]) {
  assert.throws(() => crypto.randomBytes(1, bad), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE', message: /"callback"/ });
}

let zeroBytesCalled = false;
crypto.randomBytes(0, (err, bytes) => {
  zeroBytesCalled = true;
  assert.strictEqual(err, null);
  assert.strictEqual(bytes.length, 0);
});
assert.strictEqual(zeroBytesCalled, true);

const aliasProbe = `
  const crypto = require('node:crypto');
  const warnings = [];
  process.on('warning', (warning) => warnings.push([warning.name, warning.code, warning.message]));
  const aliases = [crypto.pseudoRandomBytes, crypto.prng, crypto.rng];
  if (aliases.some((alias) => alias === crypto.randomBytes)) throw new Error('aliases should be wrappers');
  if (crypto.rng !== aliases[2] || aliases[0] === aliases[1]) throw new Error('alias identity');
  setImmediate(() => {
    if (warnings.length) throw new Error('warned before the first call');
    aliases.forEach((alias) => { if (alias(8).length !== 8) throw new Error('alias result'); });
    setImmediate(() => console.log(JSON.stringify(warnings)));
  });
`;
const expectedWarnings = [['DeprecationWarning', 'DEP0115', 'crypto.pseudoRandomBytes is deprecated.']];
for (const [flags, env] of [[['--pending-deprecation'], {}], [[], { NODE_PENDING_DEPRECATION: '1' }]]) {
  const probe = spawnSync(process.execPath, [...flags, '-e', aliasProbe], { encoding: 'utf8', env: { ...process.env, ...env } });
  assert.strictEqual(probe.status, 0, probe.stderr);
  assert.deepStrictEqual(JSON.parse(probe.stdout), expectedWarnings);
}

const order = [];

later('randomInt(max, cb)', (done) => {
  crypto.randomInt(3, done((err, n) => {
    order.push('randomInt');
    assert.strictEqual(err, undefined);
    assertInRange(n, 0, 3);
  }));
});

later('randomInt(min, max, cb)', (done) => {
  crypto.randomInt(-10, -8, done((err, n) => {
    assert.strictEqual(err, undefined);
    assertInRange(n, -10, -8);
  }));
});

for (const size of [1, 70000]) {
  later(`randomBytes(${size}, cb)`, (done) => {
    crypto.randomBytes(size, done((err, bytes) => {
      assert.strictEqual(err, null);
      assert.ok(Buffer.isBuffer(bytes));
      assert.strictEqual(bytes.length, size);
    }));
  });
}

later('randomFill runs off the main thread', (done) => {
  const seen = [];
  const large = new Uint8Array(256 * 1024 * 1024);
  crypto.randomFill(large, done((err, result) => {
    assert.strictEqual(err, null);
    assert.strictEqual(result, large);
    assert.deepStrictEqual(seen, ['tick', 'promise', 'immediate']);
    assertFilled(Array.from(large.subarray(large.length - 32)), 'end of large fill');
  }));
  process.nextTick(() => seen.push('tick'));
  Promise.resolve().then(() => seen.push('promise'));
  setImmediate(() => seen.push('immediate'));
});

later('randomFill keeps an unreferenced target alive', (done) => {
  crypto.randomFill(new Uint8Array(64 * 1024 * 1024), done((err, result) => {
    assert.strictEqual(err, null);
    assert.strictEqual(result.length, 64 * 1024 * 1024);
    assertFilled(Array.from(result.subarray(0, 32)), 'unreferenced target');
  }));
  for (let i = 0; i < 200000; i++) ({ garbage: i, more: [i] });
});

later('randomFill survives a transfer while queued', (done) => {
  const source = new ArrayBuffer(64 * 1024 * 1024);
  const view = new Uint8Array(source);
  crypto.randomFill(view, done((err, result) => {
    assert.strictEqual(err, null);
    assert.strictEqual(result, view);
    assert.strictEqual(view.length, 0);
  }));
  source.transfer();
  for (let i = 0; i < 200000; i++) ({ garbage: i, more: [i] });
});

later('randomFill(buf, cb)', (done) => {
  const target = new Uint8Array(32);
  crypto.randomFill(target, done((err, result) => {
    order.push('randomFill');
    assert.strictEqual(err, null);
    assert.strictEqual(result, target);
    assertFilled(Array.from(target), 'whole buffer');
  }));
});

later('randomFill(buf, offset, cb)', (done) => {
  const target = new Uint32Array(8);
  crypto.randomFill(target, 4, done((err, result) => {
    assert.strictEqual(err, null);
    assert.strictEqual(result, target);
    assertZero(bytesOf(target).slice(0, 16), 'leading elements');
    assertFilled(bytesOf(target).slice(16), 'tail elements');
  }));
});

later('randomFill(buf, offset, size, cb)', (done) => {
  const target = new Uint32Array(8);
  crypto.randomFill(target, 2, 4, done((err) => {
    assert.strictEqual(err, null);
    assertZero(bytesOf(target).slice(0, 8), 'leading elements');
    assertFilled(bytesOf(target).slice(8, 24), 'selected elements');
    assertZero(bytesOf(target).slice(24), 'trailing elements');
  }));
});

for (const make of [() => new ArrayBuffer(32), () => new SharedArrayBuffer(32), () => new DataView(new ArrayBuffer(32)), () => Buffer.alloc(32)]) {
  const target = make();
  later(`randomFill(${target.constructor.name}, offset, cb)`, (done) => {
    crypto.randomFill(target, 16, done((err, result) => {
      assert.strictEqual(err, null);
      assert.strictEqual(result, target);
      assertZero(bytesOf(target).slice(0, 16), 'leading bytes');
      assertFilled(bytesOf(target).slice(16), 'tail bytes');
    }));
  });
}

let zeroSizeCalled = false;
const zeroTarget = new Uint8Array(4);
crypto.randomFill(zeroTarget, 4, (err, result) => {
  zeroSizeCalled = true;
  assert.strictEqual(err, null);
  assert.strictEqual(result, zeroTarget);
});
assert.strictEqual(zeroSizeCalled, true);

order.push('sync');
setImmediate(() => {
  assert.strictEqual(order[0], 'sync');
  assert.ok(order.includes('randomInt'), order.join());
});

process.on('exit', () => {
  assert.deepStrictEqual(failures, []);
  assert.strictEqual(pending, 0);
  console.log('PASS');
});
