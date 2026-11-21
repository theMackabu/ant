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
hot(() => attempt(function () { let n = 1; var o = { set x(q) { n = 10n } }; o.x = 1; n += 1; return n }), 'THROW TypeError', 'compound add reloads a captured local');
hot(() => attempt(function () { var n = undefined; var r = []; r.push(n << 1 + n); g(); r.push((n += 2) + [n]); r.push(n); return r; function g() { n = null } }), '[0,"22",2]', 'compound add after a hoisted writer');
hot(() => attempt(function () { var n = O; var r = []; r.push((n += 1) + [n]); g(); r.push(n + n); r.push(n); return r; function g() { n = true } }), '["88",2,true]', 'compound add on an object-valued captured local');

console.log('ok');
