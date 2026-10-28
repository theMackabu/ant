const assert = require('node:assert');

const pieces = [
  '', 'a', 'ab', 'abc', 'abcd', 'abcdefg', 'abcdefgh', 'abcdefghijklmno', 'abcdefghijklmnop',
  'x'.repeat(23), 'y'.repeat(24), 'z'.repeat(30), 'w'.repeat(31), 'v'.repeat(32),
  'é', 'ü!', '日本', '\ud83d', '\ude00', '😀', 'a\ud83d', '\ude00b', 'tail'.repeat(20),
];
const numbers = [
  0, -0, 1, -1, 9, 10, 99, 100, 12345, -98765, 2147483647, -2147483648, 2147483648, 4294967295,
  1.5, -2.25, NaN, Infinity, -Infinity, 1e21, 1e-7, 123456789012, 0.1, 5e-324,
];
const objects = [
  { toString() { return 'ts'; } },
  { valueOf() { return 42; } },
  { [Symbol.toPrimitive]() { return 'prim'; } },
  [1, 2], null, undefined, true, false,
];

function rope(k) {
  let s = 'r';
  for (let i = 0; i < k; i++) s = s + pieces[i % pieces.length] + i;
  return s;
}

const ropes = [rope(3), rope(10), rope(40)];
const all = [...pieces, ...numbers, ...ropes];

function cat(a, b) { return a + b; }
function cat3(a, b, c) { return a + b + c; }
function tpl(a, b) { return `${a}<${b}>`; }
function numl(i, s) { return i + s; }
function numr(s, i) { return s + i; }
function constl(x) { return 'k=' + x; }
function constr(x) { return x + ';'; }
function app(list) { let s = ''; for (let i = 0; i < list.length; i++) s += list[i]; return s; }
function appn(n) { let s = ''; for (let i = 0; i < n; i++) s += i + ','; return s; }
function appl(list) {
  let s = '';
  for (let i = 0; i < list.length; i++) {
    s += list[i];
    if (i % 7 === 0) s.length;
  }
  return s.length + ':' + s;
}
function lens(list) { let n = 0; for (let i = 0; i < list.length; i++) n = n * 31 + (list[i] + 'q').length | 0; return n; }
function store(list, out) { for (let i = 0; i < list.length; i++) out[i & 15] = list[i] + '!'; return out.join(','); }
function hash(s) { let x = 0; for (let i = 0; i < s.length; i++) x = (x * 33 + s.charCodeAt(i)) | 0; return s.length + '#' + x; }

function runCases() {
  let seed = 12345;
  const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
  const out = [];
  for (let round = 0; round < 40; round++) {
    const acc = [];
    for (let i = 0; i < 300; i++) {
      const a = all[rnd(all.length)], b = all[rnd(all.length)], c = all[rnd(all.length)];
      if (typeof a === 'string' || typeof b === 'string') acc.push(cat(a, b), cat3(a, b, c));
      acc.push(tpl(a, b));
      const n = numbers[rnd(numbers.length)], p = pieces[rnd(pieces.length)];
      acc.push(numl(n, p), numr(p, n), numl(i - 150, p), numr(p, (i * 7919) | 0), constl(n), constr(n), constl(p), constr(p));
      if (round > 30) {
        const o = objects[rnd(objects.length)];
        acc.push(cat(p, o), cat(o, p), tpl(o, p));
      }
    }
    out.push(hash(acc.join('|')));
    out.push(hash(app(acc.slice(0, 300))));
    out.push(appl(acc.slice(0, 100).concat(pieces)).length);
    out.push(lens(acc));
    out.push(hash(store(acc, [])));
  }
  out.push(hash(appn(5000)));
  out.push(appn(40));
  out.push(numl(-2147483648, 'x'), numr('x', 2147483647), numl(0, ''), numr('', -7));
  console.log(out.join('\n'));
}

const COMPILED = ['cat', 'tpl', 'numl', 'numr', 'constl', 'constr', 'app', 'appn', 'lens', 'store'];

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
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force opt:verify dump/vm:op-warn'],
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
    console.log(`PASS opt string concat ${name}`);
  }
  console.log('ok');
}
