const assert = require('node:assert');

function child() {
  function* counter(start) {
    let x = start;
    const pad = [start, start + 1, start + 2];
    yield () => ++x + pad.length - 3;
  }

  function* withArguments(start) {
    let x = arguments[0];
    yield () => ++x;
  }

  const closures = [];
  for (let i = 0; i < 20000; i++) {
    const make = i & 1 ? withArguments : counter;
    closures.push({ base: i * 10, calls: 0, f: make(i * 10).next().value });
  }

  let junk = null;
  for (let round = 0; round < 30; round++) {
    for (let i = 0; i < 200000; i++) junk = { round, i, next: (i & 3) ? junk : null };
    junk = null;

    for (let k = 0; k < closures.length; k += 97) {
      const entry = closures[k];
      entry.calls++;
      const got = entry.f();
      if (got !== entry.base + entry.calls) throw new Error(`closure ${k} read ${got}, expected ${entry.base + entry.calls} in round ${round}`);
    }
  }

  console.log('ok');
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;

  for (const [debug, args] of [['', []], ['gc:lazy-always', []], ['gc:lazy-always', ['--jitless']], ['gc:lazy-always opt:off', []]]) {
    const result = spawnSync(process.execPath, [...args, __filename, 'child'], {
      encoding: 'utf8',
      env: { ...base, ANT_DEBUG: debug, NO_COLOR: '1' },
      timeout: 240000,
    });
    assert.strictEqual(result.status, 0, `${debug} ${args.join(' ')}: ${result.stdout}${result.stderr.slice(-4000)}`);
    assert.strictEqual(result.stdout, 'ok\n', `${debug} ${args.join(' ')}`);
  }
}
