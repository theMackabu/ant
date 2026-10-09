// A prototype or global snapshot that goes stale inside one long-running loop
// leaves compiled code at that site: the interpreter finishes the op and the
// loop OSRs into code recompiled from the updated inline cache, instead of
// running the rest of the loop on the generic path. The exit happens with
// operands pending on the stack, a double local live and a try handler open;
// the getter throws right after the exit and a for-of follows in the loop.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
function Thing(v) { this.v = v; }
let boom = false;
Thing.prototype.get = function () { if (boom) { boom = false; throw 7; } return this.v; };
const t = new Thing(2);
globalThis.gv = 3;
function addMethod(i) { Thing.prototype['m' + i] = 1; boom = true; }
function addGlobal(i) { globalThis['g' + i] = 1; }
function methods(n, at, arr) {
  let s = 0, f = 0.5, caught = 0;
  for (let i = 0; i < n; i++) {
    try {
      s += f + (i & 3) * t.get();
    } catch (e) { caught += e; }
    for (const x of arr) s += x;
    f += 0.25;
    if (i === at) addMethod(i);
  }
  return [s, caught];
}
function globals(n, at) {
  let s = 0, f = 0.5;
  for (let i = 0; i < n; i++) {
    s += f * (i & 7) + gv;
    f += 0.125;
    if (i === at) addGlobal(i);
  }
  return s;
}
console.log(JSON.stringify([methods(200000, 100000, [1, 2, 3]), globals(200000, 100000)]));
`;
const run = (args, env = {}) => spawnSync(process.execPath, [...args, '-e', source], {
  encoding: 'utf8', env: { ...process.env, NO_COLOR: '1', ...env }, timeout: 60000,
});

const interpreted = run(['--jitless']);
const compiled = run([], { ANT_DEBUG: 'dump/vm:op-warn' });
assert.strictEqual(interpreted.status, 0, interpreted.stderr);
assert.strictEqual(compiled.status, 0, compiled.stderr);
assert.strictEqual(compiled.stdout, interpreted.stdout);

for (const name of ['methods', 'globals']) {
  const events = [...compiled.stderr.matchAll(new RegExp(`^jit: (osr compiled|stale-exit) func=${name} `, 'gm'))]
    .map(match => match[1]);
  assert.deepStrictEqual(events.slice(0, 3), ['osr compiled', 'stale-exit', 'osr compiled'],
    `${name}: the stale site should leave compiled code once and the loop should re-enter fresh code`);
  assert.strictEqual(events.filter(e => e === 'stale-exit').length, 1, `${name} left compiled code once`);
}

console.log('PASS stale snapshots in a running loop re-enter recompiled code');
