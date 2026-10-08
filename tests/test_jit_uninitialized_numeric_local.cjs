// A hoisted var that type feedback says is numeric lives in a JIT double
// register; reading it before its first write returned the register's stale
// value (e.g. arr[i] read arr[0]). Locals that some path reads while still
// unassigned now stay boxed. Output must match what Node prints.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const body = String.raw`
const out = [];
const warm = (f, ...a) => { let r; for (let k = 0; k < 6; k++) r = f(...a); return r; };
function cond(n) { const seen = []; for (let k = 0; k < n; k++) { var i; if (k % 2) i = k; seen.push(String(i)); } return seen.slice(0, 4).join(); }
out.push('cond ' + warm(cond, 3000));
function cont(n) { let s = []; for (let k = 0; k < n; k++) { var x; if (k === 0) { s.push(String(x)); continue; } x = k * 2; } return s.join() + x; }
out.push('cont ' + warm(cont, 3000));
function tryc(n) { let first; for (let k = 0; k < n; k++) { var y; try { if (k === 1) throw 0; first = first === undefined ? String(y) : first; y = k + 0.5; } catch (e) { first += '|' + String(y); } } return first; }
out.push('try ' + warm(tryc, 3000));
var gTop; let topSeen = []; for (let k = 0; k < 20000; k++) { if (k < 2) topSeen.push(String(gTop)); gTop = k; }
out.push('top ' + topSeen.join());
function letUndef(n) { let r = []; for (let k = 0; k < n; k++) { let z; if (k < 2) r.push(String(z)); z = k + 1; } return r.join(); }
out.push('let ' + warm(letUndef, 3000));
function afterLoop(n) { let s = 0; for (let k = 0; k < n; k++) s += k; var late; const before = String(late); late = s; return before + ':' + late; }
out.push('after ' + warm(afterLoop, 3000));
function incFresh(n) { var c; var d; c += 1; for (let k = 0; k < n; k++) { d = (d | 0) + 1; } return String(c) + ':' + d; }
out.push('inc ' + warm(incFresh, 3000));
function addFresh(n) { var a2; for (let k = 0; k < n; k++) a2 = k; var b2; b2++; return [a2, String(b2)].join(); }
out.push('add ' + warm(addFresh, 3000));
function counter(n) { var i; for (i = 0; i < n; i++) {} return i; }
out.push('counter ' + warm(counter, 5000));
function nested(n) { let r = ''; for (let a = 0; a < 3; a++) { var m; for (let b = 0; b < n; b++) { if (b === 0) r += String(m) + ','; m = b; } } return r; }
out.push('nested ' + warm(nested, 2000));
console.log(out.join('\n'));

const arr = [10, 20, 30]; arr.undefined = 'U';
function f(n) { let first; for (let k = 0; k < n; k++) { var i; if (k === 0) first = arr[i]; i = k % 3; } return first; }
const res = []; for (let r = 0; r < 8; r++) res.push(String(f(5000)));
console.log(res.join());
`;

const env = { ...process.env, NO_COLOR: '1' };
delete env.FORCE_COLOR;
const child = spawnSync(process.execPath, ['-e', body], { encoding: 'utf8', timeout: 60000, env });
assert.strictEqual(child.status, 0, child.stderr);
assert.strictEqual(child.stdout, "cond undefined,1,1,3\ncont undefined5998\ntry undefined|0.5\ntop undefined,0\nlet undefined,undefined\nafter undefined:4498500\ninc NaN:3000\nadd 2999,NaN\ncounter 5000\nnested undefined,1999,1999,\nU,U,U,U,U,U,U,U\n");
console.log('PASS uninitialized numeric locals read undefined');
