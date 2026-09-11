// The JIT sizes its virtual operand stack from sv_func_t.max_stack, which the
// bytecode compiler computes from the real operand depth. Before that it was
// max_locals + 64 with no bounds check, and any expression needing more
// pending operands (a wide call, a long literal) wrote past the arrays. The
// bound must also cover unreachable code: the JIT and the inliner emit ops
// after a return too (a deep expression there crashed the inliner with
// "undeclared reg").
//
// Each JIT-eligible function must actually compile (no fallback to the
// interpreter), the analysis must accept every function, and no compile may
// be abandoned for a virtual-stack overflow. Calls wider than the JIT's
// argument buffer (16) and functions with `finally` are not JIT-eligible
// today; they still exercise the interpreter's reservation.
const { spawnSync } = require('child_process');

const n = 120;
const wide = 16;
const argList = (k) => Array.from({ length: k }, (_, i) => `a+${i}`).join(',');
const nested = 'a' + Array.from({ length: n }, (_, i) => `+(a+${i}`).join('') + ')'.repeat(n);
const arr = '[' + argList(n) + ']';
const obj = '{' + Array.from({ length: n }, (_, i) => `k${i}:a+${i}`).join(',') + '}';
const deadNested = 'a' + '+(a'.repeat(6) + ')'.repeat(6);

const source = `
function sink() { return arguments.length; }
function count() { return ${wide}; }
function manyArgs(a) { return sink(${argList(n)}); }
function wideCall(a) { return count(${argList(wide)}); }
function deepExpr(a) { return ${nested}; }
function bigArr(a) { return ${arr}.length; }
function bigObj(a) { return Object.keys(${obj}).length; }
function inTry(a) { try { return sink(${argList(n)}); } catch (e) { return -1; } finally { a = 0; } }
function deadDeep(a) { return 1; return ${deadNested}; }
function callsDeadDeep(a) { return deadDeep(a) + 1; }
var r = 0;
for (var i = 0; i < 300; i++)
  r += manyArgs(i) + wideCall(i) + deepExpr(i) + bigArr(i) + bigObj(i) + inTry(i) + callsDeadDeep(i);
console.log('result ' + r);
`;

const expectedDeep = (a) => { let s = a; for (let i = 0; i < n; i++) s += a + i; return s; };
let expected = 0;
for (let i = 0; i < 300; i++) expected += n + wide + expectedDeep(i) + n + n + n + 2;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
  encoding: 'utf8',
});
if (result.error) throw result.error;
if (result.status !== 0) throw new Error(`child failed (status ${result.status}):\n${result.stderr}\n${result.stdout}`);
if (!result.stdout.includes(`result ${expected}`)) throw new Error(`bad result:\n${result.stdout}`);

const bad = /undeclared reg|operand depth analysis failed|vstack-overflow|jit: bailout/;
if (bad.test(result.stderr)) throw new Error(`JIT problem:\n${result.stderr}`);
for (const fn of ['wideCall', 'deepExpr', 'bigArr', 'bigObj', 'callsDeadDeep']) {
  if (!result.stderr.includes(`jit: compiled func=${fn} `))
    throw new Error(`${fn} was not JIT-compiled:\n${result.stderr}`);
}
console.log('jit-vstack-depth: ok');
