const assert = require('node:assert');

const warm = 3000;

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function Collection() {
  this.elms = new Array();
}
Collection.prototype.add = function (e) {
  return this.elms.push(e);
};
Collection.prototype.removeFirst = function () {
  return this.elms.pop();
};

function fillDrain(n) {
  const c = new Collection();
  let s = 0;
  for (let i = 0; i < n; i++) s += c.add(i);
  for (let i = 0; i < n + 2; i++) {
    const v = c.removeFirst();
    s += v === undefined ? 1000 : v;
  }
  return s;
}

function Variable(name, value) {
  this.value = value || 0;
  this.constraints = [];
  this.determinedBy = null;
  this.mark = 0;
  this.walkStrength = 1;
  this.stay = true;
  this.name = name;
}

function Wide(a) {
  this.a = a;
  this.b = a;
  this.c = a;
  this.d = a;
  this.e = a;
  this.f = a;
  this.g = a;
  this.h = a;
  this.i = a;
  this.j = a;
  this.k = a;
  this.l = a;
}

const names = ['n0', 'n1', 'n2', 'n3', 'n4', 'n5', 'n6', 'n7'];

function makeVariables(i) {
  const v = new Variable(names[i & 7], i);
  const w = new Wide(i);
  return v.value + v.mark + v.walkStrength + (v.stay ? 1 : 0) + v.name.length + w.l + w.i + w.e;
}

function Node(key) {
  this.key = key;
}
Node.prototype.left = null;
Node.prototype.right = null;

function link(left, right, current) {
  right.left = current;
  left.right = current;
  return (left.right === current ? 1 : 0) + (right.left === current ? 2 : 0);
}

function runCases() {
  let s = 0;
  for (let k = 0; k < warm; k++) s += fillDrain(k & 31);
  const c = new Collection();
  const frozen = Object.freeze([1, 2, 3]);
  const errors = [];
  try {
    c.add.call({ elms: frozen }, 4);
  } catch (e) {
    errors.push(e.constructor.name);
  }
  try {
    c.removeFirst.call({ elms: frozen });
  } catch (e) {
    errors.push(e.constructor.name);
  }
  try {
    c.add.call({ elms: Object.seal([1]) }, 2);
  } catch (e) {
    errors.push(e.constructor.name);
  }
  const holey = [1, , 3];
  holey.length = 5;
  const popped = [c.removeFirst.call({ elms: holey }), holey.length];
  const custom = c.removeFirst.call({ elms: { pop: () => 'custom' } });
  show('push pop', [s, errors, popped, custom, c.add.call({ elms: [] }, 'x')]);

  let v = 0;
  const keep = [];
  for (let i = 0; i < warm * 10; i++) {
    v += makeVariables(i);
    if ((i & 1023) === 0) keep.push(new Variable('k', i));
  }
  show('overflow additions', [v, keep.length, JSON.stringify(keep[2]), Object.keys(new Wide(1)).join(''), Object.keys(Object.preventExtensions(new Variable('x', 1))).length]);

  let l = 0;
  for (let i = 0; i < warm * 10; i++) {
    const nodes = [new Node(i), new Node(i + 1), new Node(i + 2)];
    if (i & 1) nodes[0].left = nodes[2];
    if (i & 2) nodes[1].right = nodes[0];
    l += link(nodes[i % 3], nodes[(i + 1) % 3], nodes[(i + 2) % 3]);
  }
  Node.prototype.left = 'proto';
  show('additions', [l, link(new Node(1), new Node(2), new Node(3)), new Node(4).left]);
  Array.prototype[3] = 'proto';
  const hh = [0, 1, 2, , ];
  show('pop hole', [c.removeFirst.call({ elms: hh }), hh.length]);
}

const COMPILED = ['fillDrain', 'makeVariables', 'link'];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn dump/opt:stats'],
    ['verify', 'opt:verify dump/vm:op-warn dump/opt:stats'],
    ['force', 'opt:force,verify dump/vm:op-warn dump/opt:stats'],
    ['off', 'opt:off dump/vm:op-warn dump/opt:stats'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
      assert.match(child.stderr, /jit: opt stats compiled=[1-9]/, `${name}: stats are printed`);
      const site = /jit: opt generic site op=PUT_FIELD func=Variable bc=\d+ line=\d+ count=(\d+)/g;
      const calls = [...child.stderr.matchAll(site)].length;
      if (!base.ANT_GC_STRESS) assert.ok(calls <= 1, `${name}: only the first overflow addition of Variable calls the runtime (${calls} sites)`);
      const linkSlow = [...child.stderr.matchAll(/jit: opt generic site op=PUT_FIELD func=link bc=\d+ line=\d+ count=(\d+)/g)];
      if (!base.ANT_GC_STRESS) assert.ok(linkSlow.every((m) => Number(m[1]) < 1000), `${name}: link stores stay inline`);
    }
    console.log(`PASS opt arrays ${name}`);
  }
}
