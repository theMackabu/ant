const assert = require('node:assert');

const FLAT_SITE_MIN_LENGTH = 33;
const N = FLAT_SITE_MIN_LENGTH + 7;
const churn = () => {
  globalThis.gc?.();
  let keep = null;
  for (let i = 0; i < 300000; i++) keep = { i, next: keep && i % 64 ? keep : null, s: 'x'.repeat(i % 16) };
  return keep;
};
const make = new Function(`return [${Array.from({ length: N }, (_, i) => i).join(',')}];`);
const pristine = Array.from({ length: N }, (_, i) => i);

function check(name, mutate) {
  assert.deepStrictEqual(mutate(make()), mutate(pristine.slice()), name);
  assert.deepStrictEqual(make(), pristine, `${name} leaked into the next literal`);
  assert.deepStrictEqual(make(), pristine, `${name} leaked into a later literal`);
}

const mutators = {
  index: (a) => { a[3] = 'x'; return a; },
  indexStrict: (a) => { 'use strict'; a[3] = 'x'; return a; },
  increment: (a) => { a[5]++; a[6] += 10; return a; },
  append: (a) => { a[N] = 'end'; return a; },
  farAppend: (a) => { a[N + 10] = 'far'; return [a.length, 45 in a, a[N + 10]]; },
  push: (a) => [a.push(1, 2), a],
  pop: (a) => [a.pop(), a],
  shift: (a) => [a.shift(), a],
  unshift: (a) => [a.unshift(-1), a],
  splice: (a) => [a.splice(2, 3, 'a', 'b'), a],
  sort: (a) => a.sort((x, y) => y - x),
  reverse: (a) => a.reverse(),
  fill: (a) => a.fill(7, 1, 4),
  copyWithin: (a) => a.copyWithin(0, 10, 15),
  lengthShrink: (a) => { a.length = 3; return a; },
  lengthGrow: (a) => { a.length = 50; return [a.length, 45 in a]; },
  shrinkThenGrow: (a) => { a.length = 3; a.length = N; return [a.length, 5 in a, a[5]]; },
  shrinkThenDefine: (a) => { a.length = 3; Object.defineProperty(a, 'length', { value: N }); return [5 in a, a[5]]; },
  shrinkThenStore: (a) => { a.length = 3; a[N - 1] = 'e'; return [5 in a, a.length, a[N - 1]]; },
  shrinkThenAppend: (a) => { a.length = 3; a[3] = 'x'; a[4] = 'y'; return [a.length, a[3], a[4], 5 in a]; },
  shrinkThenDefineAppend: (a) => { Object.defineProperty(a, 'length', { value: 3 }); a[3] = 'x'; return [a.length, a[3], 4 in a]; },
  shrinkToEmptyAppend: (a) => { a.length = 0; a[0] = 'x'; a.push('y'); return [a.length, a[0], a[1]]; },
  shrinkReadonly: (a) => { Object.defineProperty(a, 'length', { value: 3, writable: false }); a[3] = 'x'; return [a.length, 3 in a]; },
  popThenAppend: (a) => { const last = a.pop(); a[a.length] = 'x'; return [last, a.length, a[a.length - 1], a]; },
  popAll: (a) => { let sum = 0; while (a.length) sum += a.pop(); a.push('y'); return [sum, a]; },
  popThenGrow: (a) => { a.pop(); a.pop(); a.length = N; return [N - 1 in a, N - 2 in a, a.length]; },
  shrinkThenFill: (a) => { a.length = 5; a.fill('f'); a.push('g'); return a; },
  shrinkThenUnshift: (a) => { a.length = 2; a.unshift('u'); return a; },
  shrinkThenSplice: (a) => { a.length = 4; a.splice(1, 1, 'p', 'q'); return a; },
  shrinkThenReverse: (a) => { a.length = 4; a.reverse(); return a; },
  shrinkThenConcat: (a) => { a.length = 3; return a.concat(a, ['c']); },
  shrinkThenWith: (a) => { a.length = 3; return [a.with(1, 'w'), a.toReversed(), a.toSpliced(0, 1), a.toSorted((x, y) => y - x)]; },
  shrinkThenPush: (a) => { a.length = 3; a.push('p'); a.length = N; return [3 in a, 5 in a, a[3]]; },
  deleteIndex: (a) => [delete a[2], 2 in a, a.length],
  defineProperty: (a) => { Object.defineProperty(a, 1, { value: 'd', writable: false }); return [a[1], Object.getOwnPropertyDescriptor(a, 1)]; },
  reflectSet: (a) => [Reflect.set(a, 4, 'r'), a[4]],
  reflectDelete: (a) => [Reflect.deleteProperty(a, 4), 4 in a],
  reflectDefine: (a) => [Reflect.defineProperty(a, 4, { value: 'q' }), a[4]],
  assign: (a) => Object.assign(a, { 0: 'z', 1: 'y' }),
  namedProp: (a) => { a.foo = 1; return [a.foo, Object.keys(a).length]; },
  symbolProp: (a) => { const s = Symbol('s'); a[s] = 1; return a[s]; },
  destructure: (a) => { [a[0], a[1]] = [a[1], a[0]]; return a; },
  forLoop: (a) => { for (let i = 0; i < a.length; i++) a[i] = i * 2; return a; },
  freeze: (a) => { Object.freeze(a); return [Object.isFrozen(a), Object.isSealed(a), Object.isExtensible(a), Reflect.set(a, 0, 1), a[0]]; },
  seal: (a) => { Object.seal(a); a[0] = 'still'; return [Object.isSealed(a), Object.isFrozen(a), delete a[0], a[0]]; },
  preventExtensions: (a) => { Object.preventExtensions(a); a[0] = 'w'; a[N] = 'no'; return [Object.isExtensible(a), a[0], a.length]; },
  queries: (a) => [Object.isFrozen(a), Object.isSealed(a), Object.isExtensible(a), Reflect.isExtensible(a), Object.getOwnPropertyDescriptor(a, 0), Object.getOwnPropertyDescriptor(a, 'length')],
  proxySet: (a) => { const p = new Proxy(a, {}); p[0] = 'p'; p.push('q'); return a; },
  spliceCall: (a) => { Array.prototype.splice.call(a, 0, 1); return a; },
  setPrototype: (a) => { Object.setPrototypeOf(a, null); a[0] = 'n'; return [Object.getPrototypeOf(a), a[0]]; },
  hole: (a) => { a.length = N + 2; a[N + 2] = 1; return [N in a, a.length]; },
};

for (const [name, mutate] of Object.entries(mutators)) check(name, mutate);

for (const call of ['a.push()', 'a.push(...[])', 'Array.prototype.push.apply(a, [])', 'a.unshift()', 'a.splice(0, 0)']) {
  const fresh = new Function(`return [${Array.from({ length: N }, (_, i) => i).join(',')}];`);
  const a = fresh();
  new Function('a', call)(a);
  assert.strictEqual(a.length, N, call);
  assert.deepStrictEqual(a, pristine, call);
  assert.deepStrictEqual(fresh(), pristine, `${call} leaked`);
}

function hot(a, i) { a[i & 31] = i; return a; }
function hotPush(a, i) { a.push(i); return a; }
for (let i = 0; i < 20000; i++) {
  const a = make();
  hot(a, i);
  if (a[i & 31] !== i) throw new Error(`hot store lost at ${i}`);
  hotPush(make(), i);
  const b = make();
  if (b[i & 31] !== (i & 31)) throw new Error(`hot store leaked at ${i}`);
}

const kept = [];
for (let i = 0; i < 2000; i++) kept.push(make());
churn();
for (const a of kept) assert.deepStrictEqual(a, pristine);
kept.length = 0;
churn();
assert.deepStrictEqual(make(), pristine);

const strings = new Function(`return [${Array.from({ length: N }, (_, i) => JSON.stringify('s' + i)).join(',')}];`);
const s1 = strings();
churn();
assert.strictEqual(s1[39], 's39');
s1[0] = 'changed';
assert.strictEqual(strings()[0], 's0');

console.log('ok');
