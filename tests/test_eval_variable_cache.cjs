const assert = require('node:assert');

const N = 20000;
const triangle = (N * (N - 1)) / 2;

assert.strictEqual((function () { return eval('var s = 0; for (var i = 0; i < N; i++) s += i; s'); })(), triangle);
assert.strictEqual((function () { var s = 0; eval('for (var i = 0; i < N; i++) s += i;'); return s; })(), triangle);
assert.strictEqual((function () { let s = 0; eval('for (let i = 0; i < N; i++) s += i;'); return s; })(), triangle);
assert.deepStrictEqual((function () { const c = 1; return eval('var n = 0; for (var i = 0; i < 50; i++) { try { c = 2; } catch (e) { n++; } } [n, c]'); })(), [50, 1]);
assert.deepStrictEqual((function (a) { eval('for (var i = 0; i < N; i++) a = a + 1;'); return [a, arguments[0]]; })(1), [N + 1, N + 1]);
assert.deepStrictEqual((function (a) { 'use strict'; eval('for (var i = 0; i < N; i++) a = a + 1;'); return [a, arguments[0]]; })(1), [N + 1, 1]);

assert.deepStrictEqual((function () {
  return eval('var r = []; for (var i = 0; i < 3; i++) { try { r.push(late); } catch (e) { r.push(e.constructor.name); } } r');
  let late = 1;
})(), ['ReferenceError', 'ReferenceError', 'ReferenceError']);

{
  globalThis.shadowed = 'global';
  const seen = (function () {
    return eval('var r = []; for (var i = 0; i < N; i++) { if (i === N / 2) eval("var shadowed = \\"local\\""); if (i === N / 2 - 1 || i === N / 2 || i === N - 1) r.push(shadowed); } r');
  })();
  delete globalThis.shadowed;
  assert.deepStrictEqual(seen, ['global', 'local', 'local']);
}

assert.deepStrictEqual((function () {
  return eval('var r = []; var gone = 1; for (var i = 0; i < 300; i++) { if (i === 100) delete gone; if (i === 200) eval("var gone = 5"); if (i % 100 === 99) { try { r.push(gone); } catch (e) { r.push(e.constructor.name); } } } r');
})(), [1, 'ReferenceError', 5]);

assert.strictEqual((function () { var x = 1; return eval('var y = 2; eval("var z = 0; for (var i = 0; i < N; i++) z += x + y; z")'); })(), 3 * N);

{
  const bump = (function () { var count = 0; return eval('(function () { for (var k = 0; k < 10; k++) count++; return count; })'); })();
  let last;
  for (let i = 0; i < 3000; i++) last = bump();
  assert.strictEqual(last, 30000);
}

{
  function counter(start) { return eval('var count = start; (function () { count += 1; return count; })'); }
  const first = counter(0), second = counter(1000);
  let sum = 0;
  for (let i = 0; i < N; i++) sum += (i & 1 ? first : second)();
  assert.deepStrictEqual([first(), second()], [N / 2 + 1, 1000 + N / 2 + 1]);
  assert.strictEqual(sum, (N / 2) * (N / 2 + 1) + 1000 * (N / 2));
}

{
  function depth(d) { return eval('var acc = d; if (d > 0) acc += depth(d - 1); acc'); }
  assert.strictEqual(depth(200), 20100);
}

assert.deepStrictEqual((function () {
  eval('function made() { return 1; } var total = 0; for (var i = 0; i < N; i++) total += made();');
  return [typeof made, total];
})(), ['function', N]);

assert.strictEqual((function () {
  return eval('var pick = function () { return 1; }; var t = 0; for (var i = 0; i < N; i++) { if (i === N / 2) pick = function () { return 2; }; t += pick(); } t');
})(), N / 2 + N);

assert.deepStrictEqual((function () {
  var s = 'outer';
  const inner = eval('"use strict"; var s = 0; for (var i = 0; i < N; i++) s += 1; s');
  return [inner, s, typeof i];
})(), [N, 'outer', 'undefined']);

assert.deepStrictEqual((function () { var a = 0, b = 0; eval('for (var i = 0; i < 2000; i++) [a, b] = [b + 1, a];'); return [a, b]; })(), [1000, 1000]);
assert.deepStrictEqual((function () { var x = 1; eval('var y = 1; for (var i = 0; i < 2000; i++) { x += 2; x -= 1; y++; --y; }'); return [x, typeof y]; })(), [2001, 'number']);
assert.deepStrictEqual((function () {
  return eval('var keep = []; var o = { v: 0 }; for (var i = 0; i < 60000; i++) { o = { v: o.v + 1 }; if (i % 1000 === 0) keep.push(o); } [o.v, keep.length, keep[5].v]');
})(), [60000, 60, 5001]);

{
  function shared(start) { eval('var count = ' + start); return function () { count += 1; return count; }; }
  const low = shared(0), high = shared(1000);
  let sum = 0;
  for (let i = 0; i < N; i++) sum += (i & 1 ? low : high)();
  assert.deepStrictEqual([low(), high()], [N / 2 + 1, 1000 + N / 2 + 1]);
  assert.strictEqual(sum, (N / 2) * (N / 2 + 1) + 1000 * (N / 2));
}

{
  function reader(value) { var local = value; eval(''); return function () { return local; }; }
  const one = reader(1), two = reader(2);
  let sum = 0;
  for (let i = 0; i < N; i++) sum += (i & 1 ? one : two)();
  assert.strictEqual(sum, (N / 2) * 3);
}

console.log('ok');
