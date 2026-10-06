// `i++` / `i--` in a for-loop update compile to an in-place local update.
// It must apply ToNumeric like the postfix operators: strings and other
// primitives convert, objects call valueOf once per step, BigInt stays BigInt
// and a Symbol throws. Checked in the interpreter and after compiling.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

function up(start, end) { const seen = []; for (let i = start; i <= end; i++) seen.push(i); return seen.join(','); }
function down(start, end) { const seen = []; for (var i = start; i >= end; i--) seen.push(typeof i); return seen.join(','); }
// the body runs once, then the update runs exactly once
function step(start) { let i = start, n = 0; for (; n < 1; i++) n++; return i; }
function stepDown(start) { let i = start, n = 0; for (; n < 1; i--) n++; return i; }

function check(when) {
  same(up('1', 4), '1,2,3,4', `${when} string up`);
  same(up(' 2 ', 3), ' 2 ,3', `${when} padded string`);
  same(up('x', 4), '', `${when} non-numeric string`);
  same(down('3', 1), 'string,number,number', `${when} string down`);
  same(up(true, 3), 'true,2,3', `${when} boolean`);
  same(up(null, 1), ',1', `${when} null`);
  same(step(undefined), NaN, `${when} undefined`);
  same(step(null), 1, `${when} null step`);
  same(step('2.5'), 3.5, `${when} decimal string`);
  same(stepDown(''), -1, `${when} empty string`);
  same(up(1n, 3n), '1,2,3', `${when} bigint`);
  same(step(1n), 2n, `${when} bigint step`);
  same(stepDown(0n), -1n, `${when} bigint down step`);
  same(down(2n, 0n), 'bigint,bigint,bigint', `${when} bigint down`);
  let calls = 0;
  const counter = { valueOf() { calls++; return 1; } };
  same(step(counter), 2, `${when} valueOf`);
  same(calls, 1, `${when} valueOf called once`);
  same(step(new Date(5)), 6, `${when} date`);
  same(step({ valueOf: () => 1n }), 2n, `${when} valueOf returning bigint`);
  same(step({ [Symbol.toPrimitive]: (hint) => (hint === 'number' ? 1 : 10) }), 2, `${when} number hint`);
  let threw = false;
  try { step(Symbol('s')); } catch (e) { threw = e instanceof TypeError; }
  same(threw, true, `${when} symbol throws TypeError`);
}

check('interpreter');
for (let i = 0; i < 3000; i++) { up(0, 3); down(3, 0); step(i); stepDown(i); }
check('compiled');
same(up(-2, 1), '-2,-1,0,1', 'numbers still work');

console.log('PASS for-loop update applies ToNumeric');
