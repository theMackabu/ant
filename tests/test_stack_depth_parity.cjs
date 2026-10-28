const assert = require('node:assert');

function recurse(n) {
  return n === 0 ? 0 : recurse(n - 1) + 1;
}

class Node {
  constructor(n) {
    this.next = n > 0 ? new Node(n - 1) : null;
  }
}

function Link(n) {
  this.next = n > 0 ? new Link(n - 1) : null;
}

function chainLength(c) {
  let d = 0;
  while (c) { d++; c = c.next; }
  return d;
}

function nestedArray(n) {
  let a = [];
  for (let i = 0; i < n; i++) a = [a];
  return a;
}

function nestedObject(n) {
  let o = {};
  for (let i = 0; i < n; i++) o = { o };
  return o;
}

function overflows(label, fn) {
  let error = null;
  try { fn(); } catch (e) { error = e; }
  assert.ok(error instanceof RangeError, `${label}: expected RangeError, got ${error}`);
  assert.match(error.message, /call stack size exceeded/, label);
}

function runCases() {
  const { limit } = Ant.stats().cstack;
  assert.ok(limit >= 20 * 1024 * 1024, `native stack limit ${limit} is below the 32 MB main stack budget`);

  assert.strictEqual(recurse(50000), 50000);
  assert.strictEqual(chainLength(new Node(10000)), 10001);
  assert.strictEqual(chainLength(new Link(10000)), 10001);

  overflows('plain recursion', () => { const f = () => f() + 1; f(); });
  overflows('new chain', () => { class C { constructor() { this.c = new C(); } } new C(); });
  overflows('eval recursion', () => { function g() { return eval('g()') + 1; } g(); });
  overflows('Function recursion', () => { const F = new Function('F', 'return F(F) + 1;'); F(F); });
  overflows('getter recursion', () => { const o = { get x() { return this.x + 1; } }; o.x; });
  overflows('JSON.parse', () => JSON.parse('['.repeat(1e6) + ']'.repeat(1e6)));
  overflows('Array#join', () => nestedArray(1e6).join());
  overflows('structuredClone', () => structuredClone(nestedObject(1e6)));
  overflows('parser', () => new Function('('.repeat(1e6) + '1' + ')'.repeat(1e6)));
  overflows('statement parser', () => new Function('{'.repeat(1e6) + '}'.repeat(1e6)));
  overflows('compiler', () => new Function('return ' + '{a:'.repeat(40000) + '1' + '}'.repeat(40000)));

  assert.strictEqual(recurse(1000), 1000);
  assert.strictEqual(chainLength(new Node(1000)), 1001);
  console.log('ok');
}

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env, NO_COLOR: '1' };
  delete base.ANT_DEBUG;
  const modes = [
    ['default', [], {}],
    ['jitless', ['--jitless'], {}],
    ['opt:force', [], { ANT_DEBUG: 'opt:force' }],
  ];
  for (const [name, args, env] of modes) {
    const child = spawnSync(process.execPath, [...args, __filename, 'child'], {
      encoding: 'utf8',
      env: { ...base, ...env },
      timeout: 120000,
      maxBuffer: 64 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.signal || ''}\n${child.stdout}${child.stderr}`);
    assert.strictEqual(child.stdout.trim(), 'ok', name);
    console.log(`PASS stack depth parity ${name}`);
  }
}
