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
function T() { throw new Error('t'); }
const O = { valueOf() { return 7; } };
const Q = x => x;
const wv = [3, 'W', 4n, O, undefined, 1.5, null, false];
function json(v) { return JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x + 'n' : x === undefined ? 'undefined' : typeof x === 'number' && x !== x ? 'NaN' : x); }
const fixed = [
  ["let n = 1; return n + 1 + (n = 's', 2) + n", "\"4s\"", "\"4s\""],
  ["let n = 1; return (n = 's', 2) + n", "\"2s\"", "\"2s\""],
  ["let n = 1; return n + ((n = 's'), n)", "\"1s\"", "\"1s\""],
  ["let n = 1; return 2 * (n = 's', 3) * n", "\"NaN\"", "\"NaN\""],
  ["let n = 1; for (var i = 0; i < 2; i++) { var x = n + 1; n = 's' } return x", "\"s1\"", "\"s1\""],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { r.push(n * 2); if (i == 1) n = 'q' } return r", "[2,2,\"NaN\"]", "[2,2,\"NaN\"]"],
  ["let a = 1, b = 2; return a + b + (a = 'x', b = 'y', 0) + a + b", "\"3xy\"", "\"3xy\""],
  ["var a = 2; return a + (a = 's', 1) + a", "\"3s\"", "\"3s\""],
  ["let n = 1; for (var i = 0; i < 2; i++) { var x = n + 1; eval('n = \"s\"') } return x", "\"s1\"", "\"s1\""],
  ["let n = 1; for (var i = 0; i < 2; i++) { var x = n + 1; var f = () => { n = 's' }; f() } return x", "\"s1\"", "\"s1\""],
  ["let f = null; const g = () => { if (f) f() }; var r = []; for (var i = 0; i < 2; i++) { var x = 1; g(); r.push(x + 1); f = () => { x = 's' } } return r", "[2,\"s1\"]", "[2,\"s1\"]"],
  ["var r = []; n = '5'; l: { if (c) break l; var n = 1 } r.push(n + n); return r", "[2]", "[\"55\"]"],
  ["var r = []; if (c) { var n = 1 } else { n = 's' } r.push(n - 1, n + 1); return r", "[\"NaN\",\"s1\"]", "[0,2]"],
  ["var r = []; switch (c) { case 1: var n = 2; break; case 0: n = 'z' } r.push(n * 2, n + 1); return r", "[\"NaN\",\"z1\"]", "[4,3]"],
  ["var r = []; try { if (!c) throw 0; var n = 1 } catch (e) { n = 'e' } r.push(n + 1); return r", "[\"e1\"]", "[2]"],
  ["var r = []; for (var i = 0; i < 3; i++) { if (i > 0) r.push(n + 1); var n = 1; if (i == 1) n = 'w' } return r", "[2,\"w1\"]", "[2,\"w1\"]"],
  ["var r = []; for (var i = 0; i < 3; r.push(n + 1), i++) { if (i == 0) continue; var n = 1 } return r", "[\"NaN\",2,2]", "[\"NaN\",2,2]"],
  ["let n = 1; var r = []; do { r.push(n + 1 + n * 2 - n / 4); ({ n } = { n: 10n }) } while (false); r.push(typeof n); return r", "[3.75,\"bigint\"]", "[3.75,\"bigint\"]"],
  ["let n = 0; var r = []; for (var i = 0; i < 3; i++) { r.push(n * 2 + 1); n = '5' } return r", "[1,11,11]", "[1,11,11]"],
  ["let n = 1; var r = []; var t = 1; for (var i = 0; i < 3; i++) { let u = t * 2; t = u + i; if (i == c) t = '5'; r.push(u, t + 1) } return r", "[2,\"51\",10,12,22,25]", "[2,3,4,\"51\",10,13]"],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { const u = n * 2; r.push(typeof u, u + 1); n = 10n } return r.map(String)", "THROW TypeError", "THROW TypeError"],
  ["let n = 1; var t = 0; for (var i = 0; i < 4; i++) { t = t + n; t = t * 2; if (i == 2) n = { valueOf() { return 5 } } } return t", "38", "38"],
  ["let h = 7; var r = []; for (var i = 0; i < 3; i++) { r.push(h * 31); h = (h * 31 + i) | 0; h = h % 1000 } return r", "[217,6727,22568]", "[217,6727,22568]"],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { r.push(n - 1); n++; n += 2; n *= 2 } return r", "[0,7,21]", "[0,7,21]"],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { r.push(n + 1); n += 's' } return r", "[2,\"1s1\",\"1ss1\"]", "[2,\"1s1\",\"1ss1\"]"],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { r.push(n + 1); n ||= 's'; n &&= 't' } return r", "[2,\"t1\",\"t1\"]", "[2,\"t1\",\"t1\"]"],
  ["let n = 1; var r = []; for (var k in { a: 1, b: 2 }) { r.push(n * 2 + 1); for (n of [null]); } return r", "[3,1]", "[3,1]"],
  ["let n = 1; var r = []; for (var i = 0; i < 2; i++) { for (var j = 0; j < 2; j++) { r.push(n + 1); if (j == c) [n] = ['d'] } } return r", "[2,\"d1\",\"d1\",\"d1\"]", "[2,2,\"d1\",\"d1\"]"],
  ["let n = 1; var r = []; var o = { set x(v) { n = 's' } }; for (var i = 0; i < 2; i++) { r.push(n + (o.x = 2) + n) } return r", "[\"3s\",\"s2s\"]", "[\"3s\",\"s2s\"]"],
  ["let n = 1; var r = []; with ({ n: 'w' }) { r.push(n + 1) } r.push(n + 1); return r", "[\"w1\",2]", "[\"w1\",2]"],
  ["var r = []; (function (n) { r.push(n + 1); arguments[0] = 's'; r.push(n + 1) })(1); return r", "[2,\"s1\"]", "[2,\"s1\"]"],
  ["function* g() { let n = 1; yield n + 1; n = 's'; yield n + 1 } return [...g()]", "[2,\"s1\"]", "[2,\"s1\"]"],
  ["let n = 1; var r = []; for (var i = 0; i < 3; i++) { r.push(-n, +n === +n, ~n); n = i ? 'x' : 2 } return r", "[-1,true,-2,-2,true,-3,\"NaN\",false,-1]", "[-1,true,-2,-2,true,-3,\"NaN\",false,-1]"],
  ["let n = 1; var r = []; for (var i = 0; i < 2; i++) { r.push(n++ + n, n-- - n); n = 'p' } return r", "[3,1,\"NaN\",\"NaN\"]", "[3,1,\"NaN\",\"NaN\"]"],
  ["let a = 1, b = 2; var r = []; for (var i = 0; i < 3; i++) { r.push(a + b); a = a + b; b = b + a; if (i == 1) b = 'B' } return r", "[3,8,\"8B\"]", "[3,8,\"8B\"]"],
  ["let n = 1; var r = []; for (var i = 0; i < 2; i++) { try { r.push(n + 1); n = 's'; T() } catch (e) { r.push(n + 1) } finally { r.push(n * 1) } } return r", "[2,\"s1\",\"NaN\",\"s1\",\"s1\",\"NaN\"]", "[2,\"s1\",\"NaN\",\"s1\",\"s1\",\"NaN\"]"],
];
for (const [src, expect0, expect1] of fixed) {
  const f = new Function('c', 'T', src);
  const run = c => { try { return json(f(c, T)); } catch (e) { return 'THROW ' + e.name; } };
  for (let i = 0; i < 160; i++) {
    assert.strictEqual(run(0), expect0, src + ' c=0 run ' + i);
    assert.strictEqual(run(1), expect1, src + ' c=1 run ' + i);
  }
}

let s = 1;
function rnd(n) { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s % n; }
function pick(a) { return a[rnd(a.length)]; }
const vals = ['1', '2.5', "'s'", '10n', 'O', 'undefined', 'true', 'null', '7', '-0', "'5'", 'w'];
const inits = ['1', '1', '1', '2.5', '0', "'s'", '10n', 'O', 'undefined', 'true', 'null', '', 'v'];
const bin = ['+', '-', '*', '/', '%', '**', '|', '&', '^', '<<', '>>', '>>>', '<', '<=', '==', '==='];
function use() {
  switch (rnd(12)) {
    case 0: return 'n ' + pick(bin) + ' ' + pick(['3', '1', "'q'", '2n', 'n']);
    case 1: return pick(['3', '1', "'q'", '2n']) + ' ' + pick(bin) + ' n';
    case 2: return pick(['-n', '+n', '~n', 'typeof n', '!n']);
    case 3: return pick(['n++', 'n--', '++n', '--n']) + ' + [n]';
    case 4: return 'n ' + pick(bin) + ' 1 ' + pick(bin) + ' n';
    case 5: return 'n + 1 + n * 2 - n / 4';
    case 6: return '(n ' + pick(['+=', '-=', '*=', '/=', '%=', '|=', '**=']) + ' ' + pick(['1', '2', "'z'"]) + ') + [n]';
    case 7: return 'n + n';
    case 8: return 'n * 2 + 1';
    case 9: return 'n - 1';
    case 10: return '1 + n + 1';
    default: return 'n ' + pick(bin) + ' ' + pick(bin.length ? ['3', '0.5'] : []);
  }
}
function asg(A) {
  switch (rnd(12)) {
    case 0: case 1: case 2: case 3: return 'n = ' + A;
    case 4: return '[n] = [' + A + ']';
    case 5: return '({ n } = { n: ' + A + ' })';
    case 6: return 'n ||= ' + A;
    case 7: return 'n ??= ' + A;
    case 8: return 'n &&= ' + A;
    case 9: return 'n += ' + A;
    case 10: return 'for (n of [' + A + ']);';
    default: return 'for (n in { k' + rnd(3) + ': 1 });';
  }
}
function gen() {
  const D = pick(['let', 'let', 'var']), I = pick(inits), A = pick(vals), U = use(), U2 = use(), S = asg(A);
  const decl = D + ' n' + (I === '' ? '' : ' = Q(' + I + ')') + '; var r = [];';
  const end = ' r.push(n); return r';
  switch (rnd(50)) {
    case 0: return decl + S + '; r.push(' + U + ');' + end;
    case 1: return decl + 'r.push(' + U + '); ' + S + '; r.push(' + U2 + ');' + end;
    case 2: return decl + 'for (var i = 0; i < 3; i++) { r.push(' + U + '); ' + S + ' }' + end;
    case 3: return decl + 'for (var i = 0; i < 4; i++) { r.push(' + U + '); if (i == 1) { ' + S + ' } r.push(' + U2 + ') }' + end;
    case 4: return decl + 'var i = 0; while (i++ < 3) { r.push(' + U + '); ' + S + ' }' + end;
    case 5: return decl + 'var i = 0; do { r.push(' + U + '); ' + S + ' } while (i++ < 2);' + end;
    case 6: return decl + 'if (c) { ' + S + ' } r.push(' + U + ');' + end;
    case 7: return decl + 'if (c) { } else { ' + S + ' } r.push(' + U + ');' + end;
    case 8: return decl + 'var f = () => { ' + S + ' }; r.push(' + U + '); f(); r.push(' + U2 + ');' + end;
    case 9: return decl + 'r.push(' + U + '); g(); r.push(' + U2 + ');' + end + '; function g() { ' + S + ' }';
    case 10: return decl + 'switch (c) { case 0: ' + S + '; case 1: r.push(' + U + '); break; default: ' + S + ' } r.push(' + U2 + ');' + end;
    case 11: return decl + 'try { ' + S + '; T() } catch (e) { r.push(' + U + ') } finally { r.push(' + U2 + ') }' + end;
    case 12: return decl + 'try { if (c) T(); r.push(' + U + ') } catch (e) { ' + S + ' } r.push(' + U2 + ');' + end;
    case 13: return decl + 'try { r.push(' + U + ') } finally { ' + S + ' } r.push(' + U2 + ');' + end;
    case 14: return decl + 'r.push((' + U + ') + (' + S.replace(/;$/, '') + ', 0) + (' + U2 + '));' + end;
    case 15: return decl + 'r.push([' + U + ', n = ' + A + ', ' + U2 + ']);' + end;
    case 16: return decl + 'l: { if (c) break l; ' + S + ' } r.push(' + U + ');' + end;
    case 17: return decl + 'for (var i = 0; i < 3; i++) { if (i == c) continue; r.push(' + U + '); ' + S + ' }' + end;
    case 18: return 'var r = []; if (c) { var n = Q(' + (I || '1') + ') } else { n = ' + A + ' } r.push(' + U + ');' + end;
    case 19: return 'var r = []; n = ' + A + '; l: { if (c) break l; var n = Q(' + (I || '1') + ') } r.push(' + U + ');' + end;
    case 20: return decl + 'eval(' + JSON.stringify(S) + '); r.push(' + U + ');' + end;
    case 21: return decl + 'with ({ n: ' + A + ' }) { r.push(' + U + ') } r.push(' + U2 + ');' + end;
    case 22: return 'return (function (n) { var r = []; r.push(' + U + '); arguments[0] = ' + A + '; r.push(' + U2 + ');' + end + ' })(' + (I || '1') + ')';
    case 23: return 'function* g() { ' + D + ' n' + (I === '' ? '' : ' = Q(' + I + ')') + '; yield ' + U + '; ' + S + '; yield ' + U2 + '; yield n } return [...g()]';
    case 24: return 'var r = []; try { throw ' + (I || '1') + ' } catch (n) { r.push(' + U + '); ' + S + '; r.push(' + U2 + '); r.push(n) } return r';
    case 25: return 'return (function (n = ' + (I || '1') + ', g = () => { ' + S + ' }) { var r = []; r.push(' + U + '); g(); r.push(' + U2 + ');' + end + ' })()';
    case 26: return decl + 'for (var i = 0; i < 2; i++) { for (var j = 0; j < 2; j++) { r.push(' + U + '); if (j == c) { ' + S + ' } } r.push(' + U2 + ') }' + end;
    case 27: return decl + 'var x = ' + U + '; ' + S + '; var y = ' + U2 + '; r.push(x, y);' + end;
    case 28: return decl + 'r.push(c ? (' + S.replace(/;$/, '').replace(/^for.*/, 'n = ' + A) + ', 1) : 2); r.push(' + U + ');' + end;
    case 29: return decl + 'r.push(c && (n = ' + A + ')); r.push(' + U + ');' + end;
    case 30: return decl + 'r.push(c || (n = ' + A + ')); r.push(' + U + ');' + end;
    case 31: return decl + 'for (var i = 0; i < 3; r.push(' + U + '), i++) { ' + S + ' }' + end;
    case 32: return decl + 'for (var i = 0; (r.push(' + U + '), i < 3); i++) { if (i == 1) { ' + S + ' } }' + end;
    case 33: return decl + 'switch (c) { case 1: var n = Q(' + (I || '1') + '); break; case 0: n = ' + A + ' } r.push(' + U + ');' + end;
    case 34: return decl + 'var o = { set x(q) { ' + S + ' } }; r.push((' + U + ') + (o.x = 1) + (' + U2 + '));' + end;
    case 35: return decl + 'r.push(' + U + '); (function () { ' + S + ' })(); r.push(' + U2 + ');' + end;
    case 36: return decl + 'for (var i = 0; i < 3; i++) { r.push(' + U + '); var f = () => { ' + S + ' }; if (i == c) f() }' + end;
    case 37: return 'var r = []; for (var i = 0; i < 3; i++) { if (i > 0) r.push(' + U + '); var n = Q(' + (I || '1') + '); r.push(' + U2 + '); if (i == c) n = ' + A + ' }' + end;
    case 38: return decl + 'for (var k in { a: 1, b: 2 }) { r.push(' + U + '); ' + S + ' }' + end;
    case 39: return decl + 'for (var q of [1, 2]) { r.push(' + U + '); if (q == 1 + c) { ' + S + ' } }' + end;
    case 40: return decl + 'try { try { T() } finally { ' + S + ' } } catch (e) { r.push(' + U + ') }' + end;
    case 41: return 'var r = []; for (var i = 0; i < 2; i++) { let n' + (I === '' ? '' : ' = Q(' + I + ')') + '; r.push(' + U + '); ' + S + '; r.push(' + U2 + ') } return r';
    case 42: return decl + 'var m = n; ' + S + '; r.push(m + 1, m * 2, ' + U + ');' + end;
    case 43: return decl + 'var t = Q(0); for (var i = 0; i < 4; i++) { t = t + n; t = t * 2; if (i == 2) { ' + S + ' } } r.push(t, ' + U + ');' + end;
    case 44: return decl + 'var t = Q(0); for (var i = 0; i < 4; i++) { t += i; if (i == 1 + c) t = ' + A + '; r.push(t + 1, t - i) } r.push(' + U + ');' + end;
    case 46: return 'var r = [], f = null; var g = () => { if (f) f() }; for (var i = 0; i < 3; i++) { var n = Q(' + (I || '1') + '); g(); r.push(' + U + '); f = () => { ' + S + ' }; r.push(' + U2 + ') }' + end;
    case 47: return 'var r = [], f = null; var g = () => { if (f) f() }; for (var i = 0; i < 3; i++) { var n = Q(' + (I || '1') + '); g(); r.push(' + U + '); f = () => { ' + S + ' }; n = 2 }' + end;
    case 48: return decl + 'var f = null; var g = () => { if (f) f() }; for (var i = 0; i < 2; i++) { for (var j = 0; j < 2; j++) { g(); r.push(' + U + '); if (j == c) f = () => { ' + S + ' } } }' + end;
    case 49: return decl + 'var f = null; var g = () => { if (f) f() }; for (var i = 0; i < 3; i++) { var m = n * 2; g(); r.push(m + 1, ' + U + '); f = () => { ' + S + ' } }' + end;
    default: return decl + 'var t = Q(1); for (var i = 0; i < 3; i++) { let u = t * 2; t = u + i; if (i == c) t = ' + A + '; r.push(u, t + 1) } r.push(' + U + ');' + end;
  }
}
let checked = 0;
for (let i = 0; i < 700; i++) {
  const src = gen();
  let typed, plain;
  try {
    typed = new Function('c', 'v', 'w', 'T', 'O', 'Q', src.replace(/Q\(([^()]*)\)/g, '$1'));
    plain = new Function('c', 'v', 'w', 'T', 'O', 'Q', src);
  } catch (e) {
    assert.ok(e instanceof SyntaxError, src);
    continue;
  }
  const c = rnd(2), v = pick(wv), w = pick(wv);
  const run = (f, cc, vv, ww) => { try { return show(f(cc, vv, ww, T, O, Q)); } catch (e) { return 'THROW ' + (e && e.name); } };
  const first = run(plain, c, v, w), other = run(plain, 1 - c, w, v);
  for (let j = 0; j < 120; j++) {
    assert.strictEqual(run(typed, c, v, w), first, src + ' run ' + j);
    if (j % 7 === 3) assert.strictEqual(run(typed, 1 - c, w, v), other, src + ' swapped run ' + j);
  }
  checked++;
}
assert.ok(checked > 600);

const outcome = (make) => {
  try {
    return { value: make() };
  } catch (error) {
    return { error: error.name };
  }
};

const worksOrRangeError = (label, make, check) => {
  const result = outcome(make);
  if (result.error) assert.strictEqual(result.error, 'RangeError', label);
  else if (check) assert.ok(check(result.value), label);
};

for (const n of [1000, 100000, 1000000]) {
  const nest = (open, inner, close) => open.repeat(n) + inner + close.repeat(n);
  const shapes = {
    'compound assignment': 'var a = 1; return ' + 'a += '.repeat(n) + '1',
  };
  for (const [label, source] of Object.entries(shapes)) {
    if (n > 1000 && label === 'compound assignment') continue;
    worksOrRangeError(`${label} ${n}`, () => new Function(source)());
  }
}

console.log('ok');
