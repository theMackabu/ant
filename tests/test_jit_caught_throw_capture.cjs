const assert = require('node:assert');

function thrower(i, value) {
  try {
    if (i % 2) throw value;
    return 1;
  } finally {
    thrower.calls++;
  }
}
thrower.calls = 0;

function middle(i, value) {
  return thrower(i, value) + 1;
}

function caughtByCaller(n, values) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    const value = values[i % values.length];
    try {
      c += middle(i, value);
    } catch (e) {
      c += Object.is(e, value) ? 3 : 1000000;
    }
  }
  return c;
}

function rethrower(i) {
  try {
    return thrower(i, i);
  } finally {
    rethrower.calls++;
  }
}
rethrower.calls = 0;

function caughtAfterFinally(n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      c += rethrower(i);
    } catch (e) {
      c += e === i ? 3 : 1000000;
    }
  }
  return c;
}

function plain(i, value) {
  let a = i | 0, b = 7;
  a = (a * 3 + 1) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 4 + 2) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 5 + 3) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 6 + 4) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 7 + 5) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  a = (a * 3 + 6) | 0; b = (b ^ a) | 0; a = (a + (b >>> 6)) | 0;
  a = (a * 4 + 7) | 0; b = (b ^ a) | 0; a = (a + (b >>> 7)) | 0;
  a = (a * 5 + 8) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 6 + 9) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 7 + 10) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 3 + 11) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 4 + 12) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  a = (a * 5 + 13) | 0; b = (b ^ a) | 0; a = (a + (b >>> 6)) | 0;
  a = (a * 6 + 14) | 0; b = (b ^ a) | 0; a = (a + (b >>> 7)) | 0;
  a = (a * 7 + 15) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 3 + 16) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 4 + 17) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 5 + 18) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 6 + 19) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  a = (a * 7 + 20) | 0; b = (b ^ a) | 0; a = (a + (b >>> 6)) | 0;
  a = (a * 3 + 21) | 0; b = (b ^ a) | 0; a = (a + (b >>> 7)) | 0;
  a = (a * 4 + 22) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 5 + 23) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 6 + 24) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 7 + 25) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 3 + 26) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  a = (a * 4 + 27) | 0; b = (b ^ a) | 0; a = (a + (b >>> 6)) | 0;
  a = (a * 5 + 28) | 0; b = (b ^ a) | 0; a = (a + (b >>> 7)) | 0;
  a = (a * 6 + 29) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 7 + 30) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 3 + 31) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 4 + 32) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 5 + 33) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  a = (a * 6 + 34) | 0; b = (b ^ a) | 0; a = (a + (b >>> 6)) | 0;
  a = (a * 7 + 35) | 0; b = (b ^ a) | 0; a = (a + (b >>> 7)) | 0;
  a = (a * 3 + 36) | 0; b = (b ^ a) | 0; a = (a + (b >>> 1)) | 0;
  a = (a * 4 + 37) | 0; b = (b ^ a) | 0; a = (a + (b >>> 2)) | 0;
  a = (a * 5 + 38) | 0; b = (b ^ a) | 0; a = (a + (b >>> 3)) | 0;
  a = (a * 6 + 39) | 0; b = (b ^ a) | 0; a = (a + (b >>> 4)) | 0;
  a = (a * 7 + 40) | 0; b = (b ^ a) | 0; a = (a + (b >>> 5)) | 0;
  if (i % 2) throw value;
  return a === b ? 1 : 1;
}

function relay(i, value) {
  return plain(i, value);
}

function caughtFromRelay(n, values) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    const value = values[i % values.length];
    try {
      c += relay(i, value);
    } catch (e) {
      c += Object.is(e, value) ? 3 : 1000000;
    }
  }
  return c;
}

function callback(value) {
  let seen = 0;
  try {
    seen = relay(value, value);
  } catch (e) {
    seen = e === value ? 2 : 1000000;
  }
  return seen + relay(value % 3 ? 0 : 1, value);
}

function caughtAcrossNative(n) {
  let c = 0;
  const one = [0];
  for (let i = 0; i < n; i++) {
    one[0] = i;
    try {
      one.forEach(callback);
      c += 1;
    } catch (e) {
      c += e === i && i % 3 === 0 ? 3 : 1000000;
    }
  }
  return c;
}

const values = [1, 'text', null, undefined, 2.5, { plain: true }, [1], Symbol.for('s'), 10n, true];

function runCaught() {
  let total = 0;
  for (let round = 0; round < 20; round++) total += caughtByCaller(2000, values) + caughtAfterFinally(2000) + caughtFromRelay(2000, values);
  return total;
}

function runNative() {
  let total = 0;
  for (let round = 0; round < 20; round++) total += caughtAcrossNative(2000);
  return total;
}

if (process.argv[2] === 'caught') {
  console.log(runCaught());
} else if (process.argv[2] === 'native') {
  console.log(runNative());
} else {
  assert.strictEqual(runCaught(), 20 * (2000 * 2.5 + 2000 * 2 + 2000 * 2));
  const nativeTotal = 20 * (667 * 3 + 1333);
  assert.strictEqual(runNative(), nativeTotal);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  if (typeof Ant !== 'undefined' && tier) {
    const { spawnSync } = require('node:child_process');
    const run = (mode) => {
      const child = spawnSync(process.execPath, [__filename, mode], {
        encoding: 'utf8',
        env: { ...process.env, ANT_DEBUG: 'dump/opt:stats', NO_COLOR: '1' },
        timeout: 120000,
      });
      assert.strictEqual(child.status, 0, child.stdout + child.stderr.slice(-4000));
      const captures = / throw_captures=(\d+) /.exec(child.stderr);
      assert.ok(captures, child.stderr.slice(-2000));
      return { out: child.stdout.trim(), captures: Number(captures[1]) };
    };
    const caught = run('caught');
    assert.strictEqual(caught.out, String(20 * (2000 * 2.5 + 2000 * 2 + 2000 * 2)));
    assert.ok(caught.captures <= 2000, 'throws a compiled caller catches captured a stack: ' + caught.captures);
    const native = run('native');
    assert.strictEqual(native.out, String(nativeTotal));
    assert.ok(native.captures >= 20 * 667 - 700 && native.captures <= 20 * 667 + 700, 'throws that cross a native frame capture, the caught ones do not: ' + native.captures);
  }
  console.log('ok');
}
