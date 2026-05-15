function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(`${label}: got ${actual}, expected ${expected}`);
  }
}

function hotConcat(seed) {
  const left = 'left:' + String(seed);
  const right = ':right:' + String(seed + 1);
  return left + right;
}

function churn(round) {
  const keep = [];
  for (let i = 0; i < 256; i++) {
    keep.push({
      tag: 'gc:' + round + ':' + i,
      payload: ['a' + i, 'b' + round, 'c' + (round + i)]
    });
  }
  return keep.length;
}

let checksum = 0;
let last = '';

for (let i = 0; i < 1200; i++) {
  last = hotConcat(i);
  checksum += last.length + churn(i);
}

assertEq(last, 'left:1199:right:1200', 'hot string concat result');
assertEq(checksum, 328983, 'checksum');

console.log('OK: test_jit_string_concat_gc_roots');
