// An array's length can be made read-only (defineProperty writable: false,
// or freezing). Length writes and index writes past the end are then rejected
// (thrown in strict code and by the Array.prototype methods), elements keep
// their own writability, and compiled push/append paths fall back. Array-like
// pop/shift/splice follow the spec's Get/Set/Delete order, as do sealed and
// non-extensible arrays and holes that read through Array.prototype. Output must match
// what Node prints.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const body = String.raw`
const out = [];
const snap = a => Array.isArray(a) ? [...a] : a;
const mk = () => { const a = [3, 1, 2]; Object.defineProperty(a, 'length', { writable: false }); return a; };
const t = (n, f, mkr = mk) => {
  const a = mkr();
  try { const v = f(a); out.push(n + "=" + JSON.stringify([v, snap(a), a.length, Object.keys(a)])); }
  catch (e) { out.push(n + "!" + e.constructor.name + JSON.stringify([snap(a), Object.keys(a)])); }
};
const ops = {
  pop: a => a.pop(), shift: a => a.shift(), unshift: a => a.unshift(9), unshift0: a => a.unshift(),
  splice: a => a.splice(0, 1), splice00: a => a.splice(0, 0), push: a => a.push(9), push0: a => a.push(),
  fill: a => a.fill(0), reverse: a => a.reverse(), sort: a => a.sort(), copyWithin: a => a.copyWithin(0, 1),
  setLen: a => { a.length = 1; return a.length; }, setLenStrict: a => { 'use strict'; a.length = 1; },
  idx3: a => { a[3] = 1; return a[3]; }, idx3strict: a => { 'use strict'; a[3] = 1; }, idx1: a => { a[1] = 7; return a[1]; },
  desc: a => Object.getOwnPropertyDescriptor(a, 'length'), redefW: a => Object.defineProperty(a, 'length', { writable: true }),
  redefSame: a => Object.defineProperty(a, 'length', { value: 3 }).length, redefDiff: a => Object.defineProperty(a, 'length', { value: 2 }),
  defIdx: a => Object.defineProperty(a, '5', { value: 1 }), isFrozen: a => Object.isFrozen(a), delIdx: a => delete a[2],
  elemDesc: a => Object.getOwnPropertyDescriptor(a, '0'),
};
for (const [name, op] of Object.entries(ops)) {
  t('ro.' + name, op);
  t('frozen.' + name, op, () => Object.freeze([3, 1, 2]));
}
const P = Array.prototype;
for (const [name, op] of Object.entries({
  pop: o => P.pop.call(o), shift: o => P.shift.call(o), splice: o => P.splice.call(o, 1, 1, 'x', 'y'), splice2: o => P.splice.call(o, 0, 3),
})) t('like.' + name, op, () => ({ length: 4, 0: 'a', 2: 'c', 3: 'd' }));
function pushes(a, n) { for (let i = 0; i < n; i++) a.push(i); return a; }
function appends(a, n) { for (let i = 0; i < n; i++) a[a.length] = i; return a; }
function appendsStrict(a, n) { 'use strict'; for (let i = 0; i < n; i++) a[a.length] = i; return a; }
for (let r = 0; r < 3000; r++) { pushes([], 8); appends([], 8); appendsStrict([], 8); }
t('jit.push', a => pushes(a, 3)); t('jit.append', a => appends(a, 3)); t('jit.appendStrict', a => appendsStrict(a, 3));
t('jit.pushFrozen', a => pushes(a, 1), () => Object.freeze([1])); t('jit.plain', a => pushes(a, 2), () => []);
console.log(out.join('\n'));
const out2 = [];
const t2 = (n, mk, f) => { const a = mk(); try { out2.push(n + '=' + JSON.stringify([f(a), [...a], a.length])); } catch (e) { out2.push(n + '!' + e.constructor.name + JSON.stringify([...a])); } };
const kinds2 = { sealed: () => Object.seal([1, 2, 3]), noext: () => Object.preventExtensions([1, 2, 3]), holey: () => [1, , 3], sealedEmpty: () => Object.seal([]) };
for (const [k, mk] of Object.entries(kinds2)) for (const [n, f] of Object.entries({
  pop: a => a.pop(), shift: a => a.shift(), del: a => a.splice(0, 1), ins: a => a.splice(1, 0, 'x'), rep: a => a.splice(1, 1, 'y'),
})) t2(k + '.' + n, mk, f);
Array.prototype[2] = 'P';
t2('protoHole.pop', () => [1, 2, ,], a => a.pop()); t2('protoHole.shift', () => [, 2, ,], a => a.shift());
delete Array.prototype[2];
console.log(out2.join('\n'));

`;

const env = { ...process.env, NO_COLOR: '1' };
delete env.FORCE_COLOR;
const child = spawnSync(process.execPath, ['-e', body], { encoding: 'utf8', timeout: 60000, env });
assert.strictEqual(child.status, 0, child.stderr);
assert.strictEqual(child.stdout, "ro.pop!TypeError[[3,1,null],[\"0\",\"1\"]]\nfrozen.pop!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.shift!TypeError[[1,2,null],[\"0\",\"1\"]]\nfrozen.shift!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.unshift!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.unshift!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.unshift0!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.unshift0!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.splice!TypeError[[1,2,null],[\"0\",\"1\"]]\nfrozen.splice!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.splice00!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.splice00!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.push!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.push!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.push0!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.push0!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.fill=[[0,0,0],[0,0,0],3,[\"0\",\"1\",\"2\"]]\nfrozen.fill!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.reverse=[[2,1,3],[2,1,3],3,[\"0\",\"1\",\"2\"]]\nfrozen.reverse!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.sort=[[1,2,3],[1,2,3],3,[\"0\",\"1\",\"2\"]]\nfrozen.sort!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.copyWithin=[[1,2,2],[1,2,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.copyWithin!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.setLen=[3,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.setLen=[3,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.setLenStrict!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.setLenStrict!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.idx3=[null,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.idx3=[null,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.idx3strict!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.idx3strict!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.idx1=[7,[3,7,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.idx1=[1,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.desc=[{\"value\":3,\"writable\":false,\"enumerable\":false,\"configurable\":false},[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.desc=[{\"value\":3,\"writable\":false,\"enumerable\":false,\"configurable\":false},[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.redefW!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.redefW!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.redefSame=[3,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.redefSame=[3,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.redefDiff!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.redefDiff!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.defIdx!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nfrozen.defIdx!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\nro.isFrozen=[false,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.isFrozen=[true,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.delIdx=[true,[3,1,null],3,[\"0\",\"1\"]]\nfrozen.delIdx=[false,[3,1,2],3,[\"0\",\"1\",\"2\"]]\nro.elemDesc=[{\"value\":3,\"writable\":true,\"enumerable\":true,\"configurable\":true},[3,1,2],3,[\"0\",\"1\",\"2\"]]\nfrozen.elemDesc=[{\"value\":3,\"writable\":false,\"enumerable\":true,\"configurable\":false},[3,1,2],3,[\"0\",\"1\",\"2\"]]\nlike.pop=[\"d\",{\"0\":\"a\",\"2\":\"c\",\"length\":3},3,[\"0\",\"2\",\"length\"]]\nlike.shift=[\"a\",{\"1\":\"c\",\"2\":\"d\",\"length\":3},3,[\"1\",\"2\",\"length\"]]\nlike.splice=[[null],{\"0\":\"a\",\"1\":\"x\",\"2\":\"y\",\"3\":\"c\",\"4\":\"d\",\"length\":5},5,[\"0\",\"1\",\"2\",\"3\",\"4\",\"length\"]]\nlike.splice2=[[\"a\",null,\"c\"],{\"0\":\"d\",\"length\":1},1,[\"0\",\"length\"]]\njit.push!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\njit.append=[[3,1,2],[3,1,2],3,[\"0\",\"1\",\"2\"]]\njit.appendStrict!TypeError[[3,1,2],[\"0\",\"1\",\"2\"]]\njit.pushFrozen!TypeError[[1],[\"0\"]]\njit.plain=[[0,1],[0,1],2,[\"0\",\"1\"]]\nsealed.pop!TypeError[1,2,3]\nsealed.shift!TypeError[2,3,3]\nsealed.del!TypeError[2,3,3]\nsealed.ins!TypeError[1,2,3]\nsealed.rep=[[2],[1,\"y\",3],3]\nnoext.pop=[3,[1,2],2]\nnoext.shift=[1,[2,3],2]\nnoext.del=[[1],[2,3],2]\nnoext.ins!TypeError[1,2,3]\nnoext.rep=[[2],[1,\"y\",3],3]\nholey.pop=[3,[1,null],2]\nholey.shift=[1,[null,3],2]\nholey.del=[[1],[null,3],2]\nholey.ins=[[],[1,\"x\",null,3],4]\nholey.rep=[[null],[1,\"y\",3],3]\nsealedEmpty.pop=[null,[],0]\nsealedEmpty.shift=[null,[],0]\nsealedEmpty.del=[[],[],0]\nsealedEmpty.ins!TypeError[]\nsealedEmpty.rep!TypeError[]\nprotoHole.pop=[\"P\",[1,2],2]\nprotoHole.shift=[null,[2,\"P\"],2]\n");
console.log('PASS read-only array length matches Node');
