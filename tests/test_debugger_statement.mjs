import assert from 'node:assert';

function outcome(src) {
  try { return 'value ' + String((0, eval)(src)) } catch (e) { return 'throws ' + e.name }
}

const cases = [
  ['1; debugger;', 'value 1'],
  ['debugger; 2', 'value 2'],
  ['debugger', 'value undefined'],
  ['debugger;', 'value undefined'],
  ['3; { debugger }', 'value 3'],
  ['3; { debugger; }', 'value 3'],
  ['20; debugger; debugger; debugger', 'value 20'],
  ['x: debugger', 'value undefined'],
  ['x: y: debugger;', 'value undefined'],
  ['8; L: { debugger; break L; }', 'value 8'],
  ['if (1) debugger; else 5', 'value undefined'],
  ['if (0) 5; else debugger', 'value undefined'],
  ['if (1) debugger\nelse 5', 'value undefined'],
  ['4; for (var i = 0; i < 2; i++) debugger', 'value undefined'],
  ['4; for (var k in { a: 1 }) debugger;', 'value undefined'],
  ['4; for (var v of [1]) debugger;', 'value undefined'],
  ['26; for (;;) { debugger; break }', 'value undefined'],
  ['label: for (;;) { debugger; break label }', 'value undefined'],
  ['9; do debugger; while (0)', 'value undefined'],
  ['9; do debugger\nwhile (0)', 'value undefined'],
  ['12; while (false) debugger', 'value undefined'],
  ['10; switch (1) { case 1: debugger }', 'value undefined'],
  ['10; switch (1) { default: debugger; 11 }', 'value 11'],
  ['11; try { debugger } finally {}', 'value undefined'],
  ['11; try { throw 1 } catch (e) { debugger }', 'value undefined'],
  ['13; with ({}) debugger', 'value undefined'],
  ['debugger\n1', 'value 1'],
  ['1\ndebugger\n2', 'value 2'],
  ['debugger\n', 'value undefined'],
  ['25; debugger\n/x/.test("x")', 'value true'],
  ['"use strict"; 7; debugger', 'value 7'],
  ['(function () { debugger; return 14 })()', 'value 14'],
  ['(function () { "use strict"; debugger; return 15 })()', 'value 15'],
  ['(function () { debugger })()', 'value undefined'],
  ['(() => { debugger; return 16 })()', 'value 16'],
  ['(function* () { debugger; yield 17 })().next().value', 'value 17'],
  ['async function a() { debugger; await 0 }; typeof a()', 'value object'],
  ['class C { static { debugger; } static m() { debugger; return 18 } }; C.m()', 'value 18'],
  ['({ get a() { debugger; return 19 } }).a', 'value 19'],
  ['({ debugger: 21 }).debugger', 'value 21'],
  ['({ debugger() { return 22 } }).debugger()', 'value 22'],
  ['class D { debugger() { return 23 } }; new D().debugger()', 'value 23'],
  ['var { debugger: ok } = { debugger: 24 }; ok', 'value 24'],
  ['var debugger = 1', 'throws SyntaxError'],
  ['let debugger = 1', 'throws SyntaxError'],
  ['const debugger = 1', 'throws SyntaxError'],
  ['"use strict"; var debugger = 1', 'throws SyntaxError'],
  ['function debugger() {}', 'throws SyntaxError'],
  ['(function debugger() {})', 'throws SyntaxError'],
  ['(function (debugger) {})', 'throws SyntaxError'],
  ['(debugger) => 1', 'throws SyntaxError'],
  ['class debugger {}', 'throws SyntaxError'],
  ['try {} catch (debugger) {}', 'throws SyntaxError'],
  ['var { debugger } = {}', 'throws SyntaxError'],
  ['debugger = 1', 'throws SyntaxError'],
  ['debugger: 1', 'throws SyntaxError'],
  ['x = debugger', 'throws SyntaxError'],
  ['debugger.x', 'throws SyntaxError'],
  ['debugger()', 'throws SyntaxError'],
  ['typeof debugger', 'throws SyntaxError'],
  ['debugger 1', 'throws SyntaxError'],
  ['debugger debugger', 'throws SyntaxError'],
  ['debugger /x/.test("x")', 'throws SyntaxError'],
  ['debu\\u0067ger', 'throws SyntaxError'],
  ['var debu\\u0067ger = 1', 'throws SyntaxError'],
];

for (const [src, expected] of cases) assert.strictEqual(outcome(src), expected, src);

let reached = 0;
debugger;
reached++;
top: debugger
reached++;
{ debugger }
reached++;
for (let i = 0; i < 3; i++) { debugger; reached++ }
assert.strictEqual(reached, 6);

assert.strictEqual(new Function('debugger; return 27')(), 27);
assert.strictEqual(new Function('"use strict"; debugger\nreturn 28')(), 28);

function hot(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    debugger;
    sum += i;
    if (i & 1) debugger;
  }
  debugger
  return sum;
}
for (let round = 0; round < 50; round++) assert.strictEqual(hot(2000), 1999000);

console.log('ok');
