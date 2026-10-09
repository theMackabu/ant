// Resuming compiled code in the interpreter rebuilds the enclosing try
// handlers. Each must restore the operand depth its TRY_PUSH saw, not the
// resumed op's: a throw caught after a bailout inside `try` used to leave the
// op's operands on the stack, and the next OSR handed one to the for-of below
// as its iterator ("iterator.next is not a function").
const assert = require('node:assert');

let boom = false;
function thr() { if (boom) { boom = false; throw 7; } return 1; }
function f(arr, n, at) {
  let s = 0, a = 3, caught = 0;
  for (let k = 0; k < n; k++) {
    let x = k;
    if (k === at) { x = 'q'; boom = true; }
    try {
      s += a * thr(x + 1);
    } catch (e) { caught += e; }
    for (const v of arr) s += v;
  }
  return [s, caught];
}

assert.deepStrictEqual(f([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 3000, 1500), [173997, 7]);
console.log('PASS resumed try handlers restore their own operand depth');
