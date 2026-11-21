const assert = require('node:assert');
function show(v, d) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'bigint') return v + 'n';
  if (typeof v === 'number') return Object.is(v, -0) ? '-0' : String(v);
  if (typeof v === 'function') return '[fn]';
  if (Array.isArray(v)) return (d || 0) > 4 ? '[..]' : '[' + v.map(x => show(x, (d || 0) + 1)).join(',') + ']';
  if (v && typeof v === 'object') return '{obj}';
  return String(v);
}
const O = { valueOf() { return 7; } };

function hot(make, expect, label) {
  for (let i = 0; i < 260; i++) assert.strictEqual(make(), expect, label + ' run ' + i);
}
function attempt(fn) { try { return show(fn()); } catch (e) { return 'THROW ' + e.name; } }
hot(() => attempt(function () { let n = 1; var r = []; var o = { set x(q) { n = 's' } }; r.push((n <= 0.5) + (o.x = 1)); r.push(n); return r }), '[1,"s"]', 'setter write survives a deopt');

function iteratorReturn() {
  let n = 0;
  const it = { [Symbol.iterator]() { return this; }, next() { return { done: false, value: 1 }; }, return() { n++; return {}; } };
  for (let i = 0; i < 3000; i++) { const [a] = it; }
  return 0 + ':' + n;
}
function getterWrite() {
  let n = 0, s = 0;
  const o = { get x() { n++; return 1; } };
  for (let i = 0; i < 3000; i++) { s += o.x; }
  return s + ':' + n;
}
function closureWrite() {
  let n = 0, s = 0;
  const inc = () => { n++; return 1; };
  for (let i = 0; i < 3000; i++) { const a = inc(); s += a; }
  return s + ':' + n;
}
for (let i = 0; i < 4; i++) {
  assert.strictEqual(iteratorReturn(), '0:3000');
  assert.strictEqual(getterWrite(), '3000:3000');
  assert.strictEqual(closureWrite(), '3000:3000');
}

console.log('ok');
