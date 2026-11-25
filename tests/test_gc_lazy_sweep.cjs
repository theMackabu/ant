const assert = require('node:assert');

function child() {
  const HOLDERS = 300000;
  const holders = new Array(HOLDERS);
  for (let i = 0; i < HOLDERS; i++) holders[i] = { id: i, ref: null, items: [i, i + 1], wide: null };

  let finalized = 0;
  const registry = new FinalizationRegistry(() => { finalized++; });
  const weak = new WeakMap();
  let checksum = 0;

  function churn(n, round) {
    let last = null;
    for (let i = 0; i < n; i++) {
      const kind = i & 15;
      if (kind === 0) last = { a: i, b: 1, c: 2, d: 3, e: 4, f: 5, g: [i] };
      else if (kind === 1) last = new Map([[i, { v: i }]]);
      else if (kind === 2) last = new Set([i, round]);
      else if (kind === 3) last = Promise.resolve(i);
      else if (kind === 4) last = [i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8];
      else if (kind === 5) { last = { key: i }; weak.set(last, { value: i }); registry.register(last, i); }
      else last = { left: last && kind > 8 ? last : null, value: i };
    }
    return last;
  }

  for (let round = 0; round < 40; round++) {
    const stride = 37 + round;
    for (let k = round % stride; k < HOLDERS; k += stride) {
      holders[k].ref = { tag: round * HOLDERS + k, payload: [k, round] };
      if ((k & 7) === 0) holders[k].wide = { a: k, b: 1, c: 2, d: 3, e: 4, f: 5, g: round };
    }

    churn(90000, round);
    if (round % 8 === 7) for (let k = 0; k < HOLDERS; k += 3) holders[k] = { id: k, ref: holders[k].ref, items: [k, k + 1], wide: holders[k].wide };

    for (let k = round % stride; k < HOLDERS; k += stride) {
      const holder = holders[k];
      if (holder.ref.tag !== round * HOLDERS + k || holder.ref.payload[0] !== k || holder.ref.payload[1] !== round)
        throw new Error(`holder ${k} lost its reference in round ${round}`);
      if ((k & 7) === 0 && (holder.wide.a !== k || holder.wide.g !== round))
        throw new Error(`holder ${k} lost its wide object in round ${round}`);
      checksum = (checksum + holder.ref.tag + holder.items[1]) | 0;
    }
  }

  for (let i = 0; i < HOLDERS; i++) {
    if (holders[i].id !== i || holders[i].items[0] !== i || holders[i].items[1] !== i + 1) throw new Error(`holder ${i} was corrupted`);
  }

  setTimeout(() => {
    console.log(checksum, finalized > 0 ? 'finalized' : 'none finalized');
  }, 20);
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;

  const run = (debug, args = []) => {
    const result = spawnSync(process.execPath, [...args, __filename, 'child'], {
      encoding: 'utf8',
      env: { ...base, ANT_DEBUG: debug, NO_COLOR: '1' },
      timeout: 240000,
      maxBuffer: 64 * 1024 * 1024,
    });
    assert.strictEqual(result.status, 0, `${debug} ${args.join(' ')}: ${result.stdout}${result.stderr.slice(-4000)}`);
    const sweeps = [...result.stderr.matchAll(/^gc: major .* lazy=(\d+)\+(\d+) /gm)].map(match => Number(match[1]) + Number(match[2]));
    return { stdout: result.stdout, sweeps };
  };

  const eager = run('gc:trace,lazy-off');
  assert.match(eager.stdout, /^-?\d+ finalized\n$/);
  assert.ok(eager.sweeps.length >= 3, `expected several major collections, saw ${eager.sweeps.length}`);
  assert.ok(eager.sweeps.every(cells => cells === 0), 'lazy-off still swept on demand');

  for (const [debug, args] of [['gc:trace', []], ['gc:trace,lazy-always', []], ['gc:trace', ['--gc-threads', '0']], ['gc:trace opt:off', []], ['gc:trace', ['--jitless']]]) {
    const lazy = run(debug, args);
    assert.strictEqual(lazy.stdout, eager.stdout, `${debug} ${args.join(' ')}`);
    assert.ok(lazy.sweeps.some(cells => cells > 100000), `${debug} ${args.join(' ')}: the lazy sweep never ran`);
  }

  console.log('ok');
}
