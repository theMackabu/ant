const assert = require('node:assert');

const failed = [];

function nest(n, target) {
  let p = target;
  for (let i = 0; i < n; i++) p = new Proxy(p, {});
  return p;
}
const deepOps = {
  get: p => p.a, set: p => { p.b = 2; return p.b; }, has: p => 'a' in p, keys: p => Object.keys(p).join(), gopd: p => typeof Object.getOwnPropertyDescriptor(p, 'a'),
  define: p => Object.defineProperty(p, 'c', { value: 1 }) === p, proto: p => Object.getPrototypeOf(p) === Object.prototype, extensible: p => Object.isExtensible(p),
  freeze: p => Object.isFrozen(Object.freeze(p)), forin: p => { let n = 0; for (const k in p) n++; return n; }, json: p => JSON.stringify(p),
  tostring: p => Object.prototype.toString.call(p), spread: p => Object.keys({ ...p }).join(), del: p => delete p.a,
};
const shallow = {};
for (const [name, op] of Object.entries(deepOps)) shallow[name] = op({ a: 1 });
for (const depth of [1000, 300000]) {
  for (const [name, op] of Object.entries(deepOps)) {
    let got;
    try { got = op(nest(depth, { a: 1 })); } catch (e) { got = e; }
    if (got instanceof RangeError && depth > 1000) continue;
    if (got !== shallow[name]) failed.push('nested ' + depth + ' ' + name + ': ' + String(got));
  }
}
for (const [name, op, want] of [['call', () => nest(300000, function () { return 7; })(), 7], ['isArray', () => Array.isArray(nest(300000, [])), true]]) {
  let got;
  try { got = op(); } catch (e) { got = e; }
  if (!(got instanceof RangeError) && got !== want) failed.push('nested ' + name + ': ' + String(got));
}

assert.deepStrictEqual(failed, []);

console.log('ok');
