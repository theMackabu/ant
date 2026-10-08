// Object.defineProperty on a mapped (sloppy) arguments index used to recurse
// through the arguments setter until the stack overflowed. A value now updates
// the parameter, accessors and writable:false unmap it, and freezing unmaps
// every index. Output must match what Node prints, except that Ant cuts the
// mapping when the function returns (the 'detached' line is excluded).
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const body = String.raw`
const out = [];
const cases = {
  value: function (a, b) { Object.defineProperty(arguments, '0', { value: 9 }); return [a, arguments[0]]; },
  valueThenParam: function (a) { Object.defineProperty(arguments, '0', { value: 9 }); a = 4; return [a, arguments[0]]; },
  roThenParam: function (a) { Object.defineProperty(arguments, '0', { value: 9, writable: false }); a = 4; return [a, arguments[0]]; },
  roThenArg: function (a) { Object.defineProperty(arguments, '0', { writable: false }); arguments[0] = 5; return [a, arguments[0]]; },
  getter: function (a) { Object.defineProperty(arguments, '0', { get() { return 'g'; } }); a = 4; return [a, arguments[0]]; },
  enumOnly: function (a) { Object.defineProperty(arguments, '0', { enumerable: false }); a = 4; arguments[0] = 6; return [a, arguments[0], Object.keys(arguments)]; },
  reflect: function (a) { const ok = Reflect.defineProperty(arguments, '0', { value: 'r' }); return [ok, a, arguments[0]]; },
  defineProps: function (a, b) { Object.defineProperties(arguments, { 0: { value: 'x' }, 1: { value: 'y', writable: false } }); b = 'z'; return [a, b, arguments[0], arguments[1]]; },
  beyond: function (a) { Object.defineProperty(arguments, '3', { value: 'w', enumerable: true }); return [a, arguments[3], arguments.length, Object.keys(arguments)]; },
  unpassed: function (a, b) { Object.defineProperty(arguments, '1', { value: 'u' }); return [a, b, arguments[1], arguments.length]; },
  strict: function (a) { 'use strict'; Object.defineProperty(arguments, '0', { value: 9 }); a = 4; return [a, arguments[0]]; },
  deleteThen: function (a) { delete arguments[0]; Object.defineProperty(arguments, '0', { value: 9 }); a = 4; return [a, arguments[0]]; },
  twice: function (a) { Object.defineProperty(arguments, '0', { value: 1 }); Object.defineProperty(arguments, '0', { value: 2 }); return [a, arguments[0]]; },
  freeze: function (a) { Object.freeze(arguments); a = 4; return [a, arguments[0], Object.isFrozen(arguments)]; },
};
for (const [n, f] of Object.entries(cases)) { try { out.push(n + ' ' + JSON.stringify(f(1))); } catch (e) { out.push(n + ' throws ' + e.name); } }
function hot(a) { Object.defineProperty(arguments, '0', { value: a + 1 }); return a; }
let s = 0; for (let i = 0; i < 20000; i++) s += hot(i); out.push('hot ' + s);
function hot2(a, b) { Object.defineProperty(arguments, 1, { get() { return 3; } }); b = 7; return arguments[1] + b; }
let s2 = 0; for (let i = 0; i < 20000; i++) s2 += hot2(i, i); out.push('hot2 ' + s2);
console.log(out.join('\n'));
function getterOnce(a) { let reads = 0; Object.defineProperty(arguments, '0', { get value() { return ++reads; } }); return [a, arguments[0], reads]; }
function getterFields(a) { const log = []; Object.defineProperty(arguments, '0', { get value() { log.push('value'); return 5; }, get writable() { log.push('writable'); return false; } }); a = 7; return [a, arguments[0], log.join()]; }
function undefinedGet(a) { Object.defineProperty(arguments, '0', { get: undefined }); a = 3; return [a, String(arguments[0])]; }
console.log(JSON.stringify([getterOnce(9), getterFields(9), undefinedGet(1)]));
`;

const env = { ...process.env, NO_COLOR: '1' };
delete env.FORCE_COLOR;
const child = spawnSync(process.execPath, ['-e', body], { encoding: 'utf8', timeout: 60000, env });
assert.strictEqual(child.status, 0, child.stderr);
assert.strictEqual(child.stdout, "value [9,9]\nvalueThenParam [4,4]\nroThenParam [4,9]\nroThenArg [1,1]\ngetter [4,\"g\"]\nenumOnly [6,6,[]]\nreflect [true,\"r\",\"r\"]\ndefineProps [\"x\",\"z\",\"x\",\"y\"]\nbeyond [1,\"w\",1,[\"0\",\"3\"]]\nunpassed [1,null,\"u\",1]\nstrict [4,9]\ndeleteThen [4,9]\ntwice [2,2]\nfreeze [4,1,true]\nhot 200010000\nhot2 200000\n[[1,1,1],[7,5,\"value,writable\"],[3,\"undefined\"]]\n");
console.log('PASS mapped arguments defineProperty matches Node');
