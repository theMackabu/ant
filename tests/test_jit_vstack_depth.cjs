// The JIT sizes its virtual operand stack from sv_func_t.max_stack, which the
// bytecode compiler computes from the real operand depth. Before that, it was
// max_locals + 64 with no bounds check, and any expression needing more
// pending operands (a wide call, a long literal) wrote past the arrays.
const { spawnSync } = require('child_process');

const n = 120;
const args = Array.from({ length: n }, (_, i) => `a+${i}`).join(',');
const nested = 'a' + Array.from({ length: n }, (_, i) => `+(a+${i}`).join('') + ')'.repeat(n);
const arr = '[' + Array.from({ length: n }, (_, i) => `a+${i}`).join(',') + ']';
const obj = '{' + Array.from({ length: n }, (_, i) => `k${i}:a+${i}`).join(',') + '}';

const source = `
function sink() { return arguments.length; }
function manyArgs(a) { return sink(${args}); }
function deepExpr(a) { return ${nested}; }
function bigArr(a) { return ${arr}.length; }
function bigObj(a) { return Object.keys(${obj}).length; }
function wideTail(a) { return sink(${args}); }
function inTry(a) { try { return sink(${args}); } catch (e) { return -1; } finally { a = 0; } }
var r = 0;
for (var i = 0; i < 300; i++) r += manyArgs(i) + deepExpr(i) + bigArr(i) + bigObj(i) + wideTail(i) + inTry(i);
console.log('result ' + r);
`;

const expectedDeep = (a) => { let s = a; for (let i = 0; i < n; i++) s += a + i; return s; };
let expected = 0;
for (let i = 0; i < 300; i++) expected += n + expectedDeep(i) + n + n + n + n;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
  encoding: 'utf8',
});
if (result.error) throw result.error;
if (result.status !== 0) throw new Error(`child failed (status ${result.status}):\n${result.stderr}\n${result.stdout}`);
if (!result.stdout.includes(`result ${expected}`)) throw new Error(`bad result:\n${result.stdout}`);
if (/undeclared reg|jit vstack/.test(result.stderr)) throw new Error(`JIT overflow:\n${result.stderr}`);
console.log('jit-vstack-depth: ok');
