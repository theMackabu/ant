const assert = require('node:assert');

const KINDS = ['microtask', 'reaction', 'tick', 'immediate', 'await', 'rejection', 'weakref'];

function child(kind) {
  const churn = count => {
    let keep = null;
    for (let i = 0; i < count; i++) { const o = { i, a: [i] }; if ((i & 4095) === 0) keep = o; }
    return keep;
  };

  let seen = 0;
  const visit = o => { seen += o.i & 1; };
  const waiter = async o => { await null; visit(o); };
  const refs = [];

  for (let i = 0; i < 400000; i++) {
    const o = { i };
    switch (kind) {
      case 'microtask': queueMicrotask(() => visit(o)); break;
      case 'reaction': Promise.resolve(o).then(visit); break;
      case 'tick': process.nextTick(visit, o); break;
      case 'immediate': setImmediate(visit, o); break;
      case 'await': waiter(o); break;
      case 'rejection': Promise.reject(o); break;
      case 'weakref': refs.push(new WeakRef(o)); break;
    }
  }

  churn(2000000);
  process.exit(0);
}

if (process.argv[2]) {
  child(process.argv[2]);
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;

  const minorCost = kind => {
    const result = spawnSync(process.execPath, [__filename, kind], {
      encoding: 'utf8',
      env: { ...base, ANT_DEBUG: 'gc:trace', NO_COLOR: '1' },
      timeout: 180000,
    });
    assert.strictEqual(result.status, 0, result.stderr.slice(-2000));
    const costs = [...result.stderr.matchAll(/gc: minor .*? ms=([\d.]+)/g)].map(match => Number(match[1]));
    if (costs.length < 40) return null;
    const tail = costs.slice(-30).sort((a, b) => a - b);
    return tail[15];
  };

  const alone = minorCost('none');
  for (const kind of KINDS) {
    const loaded = minorCost(kind);
    if (alone === null || loaded === null) continue;
    assert.ok(loaded < alone * 8 + 0.1, `${kind}: a minor collection took ${loaded} ms with 400000 pending, ${alone} ms with none`);
  }
  console.log('ok');
}
