// Compiled code snapshots a prototype's or the global object's shape. Once that
// shape changes the snapshot can never match again, so the first stale miss
// drops the compiled code and a later call recompiles from the updated inline
// cache. Objects that keep changing shape stop being snapshotted after a few
// resets instead of recompiling forever. Reads stay correct throughout.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const child = spawnSync(process.execPath, ['-e', `
function Thing(v) { this.v = v; }
Thing.prototype.get = function () { return this.v; };
const things = [new Thing(1), new Thing(2)];
function methods(n) { let s = 0; for (let i = 0; i < n; i++) s += things[i & 1].get(); return s; }
globalThis.gv = 3;
function globals(n) { let s = 0; for (let i = 0; i < n; i++) s += gv; return s; }

const out = [];
for (let r = 0; r < 50; r++) { methods(2000); globals(2000); }
Thing.prototype.extra = function () {};
globalThis.another = 1;
for (let r = 0; r < 200; r++) { out[0] = methods(2000); out[1] = globals(2000); }
Thing.prototype.get = function () { return this.v * 10; };
globalThis.gv = 4;
out.push(methods(10), globals(10));

function Churn(v) { this.v = v; }
Churn.prototype.get = function () { return this.v; };
const churned = [new Churn(1), new Churn(2)];
function churn(n) { let s = 0; for (let i = 0; i < n; i++) s += churned[i & 1].get(); return s; }
let total = 0;
for (let r = 0; r < 400; r++) { total += churn(2000); Churn.prototype['m' + r] = function () {}; }
out.push(total);
console.log(JSON.stringify(out));
`], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn', NO_COLOR: '1' }, timeout: 60000 });

assert.strictEqual(child.status, 0, child.stderr);
const compiles = name => (child.stderr.match(new RegExp(`^jit: compiled func=${name} `, 'gm')) || []).length;

assert.strictEqual(child.stdout.trim(), JSON.stringify([3000, 6000, 150, 40, 1200000]));
assert.strictEqual(compiles('methods'), 2, 'method snapshot went stale once and recompiled');
assert.strictEqual(compiles('globals'), 2, 'global snapshot went stale once and recompiled');
assert.ok(compiles('churn') <= 4, `churn recompiled ${compiles('churn')} times`);

console.log('PASS stale shape snapshots recompile once and stop churning');
