const assert = require('node:assert');

const WARM = 3000;

function scenario(name, setup, mutate) {
  const { call, before, after } = setup();
  for (let i = 0; i < WARM; i++) assert.strictEqual(call(), before, `${name}: warm-up`);
  mutate();
  const seen = [];
  for (let i = 0; i < 3; i++) seen.push(call());
  assert.deepStrictEqual(seen, [after, after, after], name);
  console.log(name, seen[0]);
}

function classOf(methods, parent) {
  function C() {}
  if (parent) C.prototype = Object.create(parent.prototype);
  for (const key of Object.keys(methods)) C.prototype[key] = methods[key];
  return C;
}

function caller(key) {
  return new Function('o', `return o.${key}();`);
}

function child() {
  scenario('assign', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 1, after: 2 };
  }, () => { globalThis.mutateTarget.prototype.m = function () { return 2; }; });

  scenario('define value', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 1, after: 3 };
  }, () => { Object.defineProperty(globalThis.mutateTarget.prototype, 'm', { value() { return 3; } }); });

  scenario('define getter', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 1, after: 4 };
  }, () => { Object.defineProperty(globalThis.mutateTarget.prototype, 'm', { get() { return () => 4; } }); });

  scenario('delete to parent', () => {
    const P = classOf({ m() { return 'parent'; } });
    const C = classOf({ m() { return 'child'; } }, P);
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 'child', after: 'parent' };
  }, () => { delete globalThis.mutateTarget.prototype.m; });

  scenario('shadow on intermediate', () => {
    const P = classOf({ m() { return 'parent'; } });
    const C = classOf({ other() { return 0; } }, P);
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 'parent', after: 'shadow' };
  }, () => { globalThis.mutateTarget.prototype.m = function () { return 'shadow'; }; });

  scenario('intermediate prototype swap', () => {
    const P = classOf({ m() { return 'parent'; } });
    const C = classOf({ other() { return 0; } }, P);
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 'parent', after: 'swapped' };
  }, () => { Object.setPrototypeOf(globalThis.mutateTarget.prototype, { m() { return 'swapped'; } }); });

  scenario('object assign', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 1, after: 5 };
  }, () => { Object.assign(globalThis.mutateTarget.prototype, { m() { return 5; } }); });

  scenario('reflect set', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 1, after: 6 };
  }, () => { Reflect.set(globalThis.mutateTarget.prototype, 'm', function () { return 6; }); });

  scenario('freeze keeps method', () => {
    const C = classOf({ m() { return 7; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 7, after: 7 };
  }, () => { Object.freeze(globalThis.mutateTarget.prototype); });

  scenario('spread copy is independent', () => {
    const C = classOf({ m() { return 8; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 8, after: 8 };
  }, () => {
    const copies = [];
    for (let i = 0; i < 50; i++) {
      const copy = { ...globalThis.mutateTarget.prototype };
      copy.m = function () { return -1; };
      copies.push(copy);
    }
    assert.strictEqual(copies[49].m(), -1);
  });

  scenario('receiver prototype change', () => {
    const C = classOf({ m() { return 1; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = o;
    return { call: () => f(o), before: 1, after: 9 };
  }, () => { Object.setPrototypeOf(globalThis.mutateTarget, { m() { return 9; } }); });

  scenario('getter redefined', () => {
    function C() {}
    Object.defineProperty(C.prototype, 'x', { get() { return 10; }, configurable: true });
    const o = new C(), f = new Function('o', 'return o.x;');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 10, after: 11 };
  }, () => { Object.defineProperty(globalThis.mutateTarget.prototype, 'x', { get() { return 11; } }); });

  scenario('symbol key added', () => {
    const C = classOf({ m() { return 12; } });
    const o = new C(), f = caller('m');
    globalThis.mutateTarget = C;
    return { call: () => f(o), before: 12, after: 12 };
  }, () => { globalThis.mutateTarget.prototype[Symbol('s')] = 1; });

  const A = classOf({ m() { return 1; } });
  const a = new A();
  let flag = false;
  const isFlag = () => flag;
  const fns = [];
  for (let j = 0; j < 8; j++) fns.push(new Function('flag', 'A', `if (flag()) A.prototype.m = function () { return 1000; }; return ${j};`));
  function inflight(o, k) {
    let s = 0;
    for (let i = 0; i < k; i++) { s += o.m(); fns[i & 7](isFlag, A); s += o.m(); }
    return s;
  }
  for (let i = 0; i < 800; i++) assert.strictEqual(inflight(a, 50), 100);
  flag = true;
  assert.strictEqual(inflight(a, 3), 5001);
  assert.strictEqual(inflight(a, 2), 4000);
  console.log('inflight', 5001);
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.match(interp.stdout, /inflight 5001\n$/);
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && name === 'default')
      assert.match(child.stderr, /func=inflight .*reason=opt-exit kind=Invalidated/, 'a running frame must exit once its code is invalidated');
    console.log(`PASS opt watched prototype ${name}`);
  }
  if (tier) {
    const dumped = run({ ANT_DEBUG: 'dump/opt:graph' });
    assert.strictEqual(dumped.status, 0, `dump: ${dumped.error || dumped.stderr.slice(-4000)}`);
    assert.strictEqual(dumped.stdout, interp.stdout, 'dump: output differs from --jitless');
    assert.match(dumped.stderr, /AssumeValid/, 'dump: watched methods must leave an AssumeValid');
    console.log('PASS opt watched prototype dump');
  }
}
