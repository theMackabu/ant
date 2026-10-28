const assert = require('node:assert');

function Plan() {
  this.v = 1;
}
Plan.prototype.size = function () {
  return this.v;
};

function Pair(a, b) {
  this.a = a;
  this.b = b;
}

function Wide(base) {
  this.p0 = base;
  this.p1 = base;
  this.p2 = base;
  this.p3 = base;
}

function Narrow(base) {
  this.p0 = base;
}

function Holder(kind) {
  this.kind = kind;
  this.tag = kind;
}

function Tagged(base) {
  Holder.call(this, base);
}

function Scaled(base) {
  this.d = base;
  this.s = base;
  this.o = base;
  Holder.call(this, base);
}

function planStep(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const p = new Plan();
    s += p.size() + p.v;
  }
  return s;
}

function pairStep(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const p = new Pair(i, 1);
    s += p.a + p.b;
  }
  return s;
}

function mixedStep(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const o = i & 1 ? new Tagged(i) : new Scaled(i);
    s += o.tag + o.kind;
  }
  return s;
}

function wideStep(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const o = i & 3 ? new Narrow(i) : new Wide(i);
    s += o.p0;
  }
  return s;
}

function churn(rounds) {
  let keep = null;
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(4096);
    for (let i = 0; i < batch.length; i++) batch[i] = { i, next: keep, text: 'x' + i };
    keep = batch[r & 4095];
    keep.next = null;
  }
  return keep.i;
}

function expected(name, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (name === 'plan') s += 2;
    else if (name === 'pair') s += i + 1;
    else if (name === 'mixed') s += 2 * i;
    else s += i;
  }
  return s;
}

const steps = { plan: planStep, pair: pairStep, mixed: mixedStep, wide: wideStep };

function runChild() {
  for (let warm = 0; warm < 400; warm++) for (const name in steps) assert.strictEqual(steps[name](64), expected(name, 64));
  console.error('PHASE steady');
  for (let round = 0; round < 12; round++) {
    churn(400);
    for (let k = 0; k < 40; k++) for (const name in steps) assert.strictEqual(steps[name](64), expected(name, 64), `${name} round ${round}`);
  }
  console.log('done');
}

if (process.argv[2] === 'child') {
  runChild();
} else {
  const { spawnSync } = require('node:child_process');
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  for (const mode of ['default', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: `dump/vm:op-warn${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 64 * 1024 * 1024,
    });
    const label = mode;
    assert.strictEqual(child.status, 0, `${label}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.match(child.stdout, /done/, `${label}: result`);
    const steady = child.stderr.slice(child.stderr.indexOf('PHASE steady'));
    assert.ok(child.stderr.includes('PHASE steady'), `${label}: phase marker`);
    if (tier && mode === 'default') {
      assert.match(child.stderr, /jit: opt compiled func=planStep/, `${label}: planStep compiles on the opt tier`);
      const drops = steady.match(/jit: (opt dropped|opt disabled|bailout).*func=(planStep|pairStep|mixedStep|wideStep|Plan|Pair|Holder|Tagged|Scaled|Wide|Narrow|<anonymous>)\b.*/g) || [];
      assert.deepStrictEqual(drops, [], `${label}: code stays valid across major collections`);
      const slow = child.stderr.match(/jit: opt (dropped|disabled) func=(Holder|Tagged|Scaled|mixedStep)\b.*/g) || [];
      assert.deepStrictEqual(slow, [], `${label}: additions that allocate overflow storage are no slow paths`);
    }
    console.log(`PASS opt shapes across collections ${label}`);
  }
}
