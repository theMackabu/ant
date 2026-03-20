function makeIter() {
  let n = 0;
  return function iter() {
    const prev = n++;
    return prev;
  };
}

const iter = makeIter();
let sum = 0;
const rounds = 5000;

for (let i = 0; i < rounds; i++) {
  sum += iter();
}

const expected = (rounds - 1) * rounds / 2;
if (sum !== expected) {
  throw new Error(
    "post-inc sum mismatch: got " + sum + ", expected " + expected
  );
}

const next = iter();
if (next !== rounds) {
  throw new Error(
    "post-inc next mismatch: got " + next + ", expected " + rounds
  );
}

console.log("OK: test_jit_post_inc_upval");
