// object spread and rest copy index keys from arrays and strings, and behave the same once compiled
function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

const same = (a, b) => JSON.stringify(Reflect.ownKeys(a).map(String)) + JSON.stringify(a) === JSON.stringify(Reflect.ownKeys(b).map(String)) + JSON.stringify(b);

const arr = [10, , 30];
arr.named = 'n';
assert(same({ ...arr }, { 0: 10, 2: 30, named: 'n' }), 'array spread copies elements, skips holes, keeps named keys');
assert(same({ ...'ab' }, { 0: 'a', 1: 'b' }), 'string spread copies code units');
assert(same({ ...'😀' }, { 0: '\ud83d', 1: '\ude00' }), 'string spread splits surrogate pairs');

const wrapped = new String('xy');
wrapped.extra = 1;
assert(same({ ...wrapped }, { 0: 'x', 1: 'y', extra: 1 }), 'String objects spread their characters and own keys');
assert(same({ ...null, ...undefined, ...5, ...true }, {}), 'nullish and primitive non-strings spread nothing');

const { 0: first, ...restArr } = [1, 2, 3];
assert(first === 1 && same(restArr, { 1: 2, 2: 3 }), 'rest from an array leaves out the named index');
const { 1: second, ...restStr } = 'abc';
assert(second === 'b' && same(restStr, { 0: 'a', 2: 'c' }), 'rest from a string leaves out the named index');

function spread(o) { return { ...o, extra: 1 }; }
function rest(o) { const { b, ...r } = o; return r; }
function guarded(o) { try { return { ...o }; } catch (e) { return e.message; } }

let s, r, g;
for (let i = 0; i < 20000; i++) {
  s = spread({ x: i, y: 2 });
  r = rest({ b: 1, c: i, d: 3 });
  g = guarded(i % 2 ? { get t() { throw new Error('boom'); } } : { k: i });
}
assert(same(s, { x: 19999, y: 2, extra: 1 }), 'compiled spread copies every key');
assert(same(r, { c: 19999, d: 3 }), 'compiled rest leaves out named keys');
assert(g === 'boom', 'compiled spread rethrows getter errors');

console.log('object spread sources OK');
