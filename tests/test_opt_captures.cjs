const assert = require('node:assert');

const warm = 4000;

const EXPECTED = [
  'adder 8002000',
  'curried 1923939',
  'six params 8058000',
  'deep 8010000',
  'mutated 16028000',
  'semantics [99,100,5,3,"function","x",120,2,10,3,3,3,[5,10]]',
  'merges 8392000',
  'loops 3143000',
  'named loop 2790000',
  'exit [7997,"7996z",7999]',
  'churn 3603714246',
  'async 42',
].join('\n') + '\n';

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function adder(a) {
  return function (b) {
    return a + b;
  };
}

function curried(a) {
  return (b) => (c) => a * 100 + b * 10 + c;
}

function sixParams(a, b, c, d, e, f) {
  return () => a + b + c + d + e + f;
}

function deep(a, o) {
  return function () {
    return function () {
      return a + o.k;
    };
  };
}

function mutatedAfter(a) {
  const f = () => a;
  a = a + 1;
  return f;
}

function mutatedInside(a) {
  const set = (v) => {
    a = v;
  };
  const get = () => a;
  set(a + 7);
  return get();
}

function sloppyArguments(a) {
  const g = () => a;
  arguments[0] = 99;
  return g();
}

function strictArguments(a) {
  'use strict';
  const g = () => a;
  arguments[0] = 99;
  return g() + arguments[0];
}

function directEval(a) {
  const g = () => a;
  eval('a = 5');
  return g();
}

function nestedEval(a) {
  const g = () => a;
  const h = () => eval('a + 1');
  return g() + h();
}

function shadowingDeclaration(a) {
  function a() {
    return 3;
  }
  return () => typeof a;
}

function forInHead(a) {
  const g = () => a;
  for (a in { x: 1 }) {}
  return g();
}

function destructured(a, b) {
  const g = () => a + b;
  [a, b] = [b * 10, a * 100];
  return g();
}

function updated(a) {
  const g = () => a;
  a++;
  return g();
}

function redeclared(a) {
  const g = () => a;
  var a = 10;
  return g();
}

function duplicate(a, a2) {
  return () => a + a2;
}

function withDefault(a, b = a + 1) {
  return () => a + b;
}

function withRest(a, ...rest) {
  return () => a + rest.length;
}

function* generator(a, b) {
  const f = () => a + b;
  yield f();
  yield f() * 2;
}

async function asyncCapture(a) {
  const f = () => a;
  await null;
  return f() + 1;
}

function boxedOnOnePath(n, flag) {
  let x = n;
  let y = n * 2;
  let r = 0;
  if (flag) {
    const f = () => x + y;
    r = f();
  } else {
    r = x - y;
  }
  return r;
}

function writeAfterMerge(n, flag) {
  let x = n;
  let f = null;
  if (flag) f = () => x;
  x = n + 100;
  return f ? f() : x;
}

function loopShared(n) {
  var total = 0;
  var fs = [];
  for (var i = 0; i < n; i++) {
    var v = i * 3;
    fs.push(() => v + total);
  }
  total = 1000;
  let s = 0;
  for (const f of fs) s += f();
  return s;
}

function loopFresh(n) {
  const fs = [];
  for (let i = 0; i < n; i++) {
    let v = i * 3;
    fs.push(() => v + i);
    v += 1;
  }
  let s = 0;
  for (const f of fs) s += f();
  return s;
}

function namedLoop(list, scale) {
  let loop;
  let acc = 0;
  loop = function (l) {
    if (l === null) return acc;
    acc += l.v * scale;
    return loop(l.next);
  };
  return loop(list);
}

function exitAfterCapture(a, k) {
  const f = () => a * 2;
  return f() + k.x;
}

function makeList(n) {
  let l = null;
  for (let i = n; i > 0; i--) l = { v: i, next: l };
  return l;
}

function churn(n) {
  let keep = [];
  let s = 0;
  for (let i = 0; i < n; i++) {
    const f = adder({ v: i }.v);
    const g = deep(i, { k: i & 7 });
    if ((i & 1023) === 0) keep.push(f, g);
    s += f(1) + g()();
  }
  for (let j = 0; j < 200000; j++) ({ j });
  for (let j = 0; j < keep.length; j += 2) s += keep[j](2) + keep[j + 1]()();
  return s;
}

function runCases() {
  let s = 0;
  for (let k = 0; k < warm; k++) s += adder(k)(1);
  show('adder', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += curried(k % 10)(k % 7)(k % 3);
  show('curried', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += sixParams(k, 1, 2, 3, 4, 5)();
  show('six params', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += deep(k, { k: 3 })()();
  show('deep', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += mutatedAfter(k)() + mutatedInside(k);
  show('mutated', s);

  show('semantics', [
    sloppyArguments(1), strictArguments(1), directEval(1), nestedEval(1), shadowingDeclaration(1)(),
    forInHead(1), destructured(1, 2), updated(1), redeclared(1), duplicate(1, 2)(), withDefault(1)(), withRest(1, 2, 3)(),
    [...generator(2, 3)],
  ]);

  s = 0;
  for (let k = 0; k < warm; k++) s += boxedOnOnePath(k, (k & 3) === 0) + writeAfterMerge(k, (k & 1) === 0);
  show('merges', s);

  s = 0;
  for (let k = 0; k < 400; k++) s += loopShared(k & 15) + loopFresh(k & 15);
  show('loops', s);

  const list = makeList(30);
  s = 0;
  for (let k = 0; k < warm; k++) s += namedLoop(list, k & 3);
  show('named loop', s);

  const shapes = [{ x: 1 }, { y: 2, x: 3 }, { x: 'z' }];
  const tail = [];
  for (let k = 0; k < warm; k++) {
    const r = exitAfterCapture(k, shapes[k < warm - 3 ? 0 : k % 3]);
    if (k >= warm - 3) tail.push(r);
  }
  show('exit', tail);

  show('churn', churn(60000));

  asyncCapture(41).then((v) => show('async', v));
}

const COMPILED = ['adder', 'sixParams', 'boxedOnOnePath', 'writeAfterMerge', 'namedLoop'];

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
  assert.strictEqual(interp.stdout, EXPECTED, 'interpreter output differs from node');

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
    }
    console.log(`PASS opt captures ${name}`);
  }
  console.log('ok');
}
