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
hot(() => attempt(function () { let n = 1; var o = { set x(q) { n = 7 } }; o.x = 1; n += 1; return n }), '8', 'compound add after a setter write');

console.log('ok');
