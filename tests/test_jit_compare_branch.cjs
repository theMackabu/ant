// A comparison followed directly by a conditional jump branches on the
// comparison in compiled code (no boxed boolean). The branch must take the
// same side as the boolean would: NaN compares false both ways, -0 equals
// 0, and non-number operands still go through the generic comparison.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

const branches = {
  lt: (a, b) => { if (a < b) return 1; return 0; },
  le: (a, b) => { if (a <= b) return 1; return 0; },
  gt: (a, b) => { if (a > b) return 1; return 0; },
  ge: (a, b) => { if (a >= b) return 1; return 0; },
  notLt: (a, b) => { if (!(a < b)) return 1; return 0; },
  ternary: (a, b) => (a < b ? 1 : 0),
  and: (a, b) => (a <= b && b >= a ? 1 : 0),
  loop: (a, b) => { let n = 0; for (let x = a; x < b && n < 5; x++) n++; return n; },
  // the jump joins two comparisons: it must test whichever ran
  join: (a, b) => { if (a > 0 ? a < b : b < a) return 1; return 0; },
};
const reference = {
  lt: (a, b) => +(a < b),
  le: (a, b) => +(a <= b),
  gt: (a, b) => +(a > b),
  ge: (a, b) => +(a >= b),
  notLt: (a, b) => +!(a < b),
  ternary: (a, b) => +(a < b),
  and: (a, b) => +(a <= b && b >= a),
  join: (a, b) => +(a > 0 ? a < b : b < a),
};

for (let i = 0; i < 5000; i++)
  for (const f of Object.values(branches)) f(i & 7, (i >> 3) & 7);

const values = [0, -0, 1, -1, 0.5, 2 ** 31, -(2 ** 53), NaN, Infinity, -Infinity, 1e-300];
for (const a of values)
  for (const b of values)
    for (const [name, f] of Object.entries(reference)) {
      // reference results come from comparisons stored, not branched on
      same(branches[name](a, b), f(a, b), `${name}(${a}, ${b})`);
    }

same(branches.loop(0, 3), 3, 'loop to 3');
same(branches.loop(0, NaN), 0, 'loop to NaN');
same(branches.loop(NaN, 3), 0, 'loop from NaN');
same(branches.loop(-Infinity, 0), 5, 'loop from -Infinity');

// non-number operands
same(branches.lt('a', 'b'), 1, 'strings');
same(branches.lt('10', 9), 0, 'string and number');
same(branches.ge({ valueOf: () => 3 }, 2), 1, 'valueOf');
same(branches.le(null, 0), 1, 'null');
same(branches.lt(undefined, 1), 0, 'undefined');
same(branches.gt(2n, 1), 1, 'bigint');

console.log('PASS compiled compare-and-branch matches the boolean comparison');
