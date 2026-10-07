// Element buffers of power-of-two capacity up to 32 are reused from a free list after
// their array dies (the next-free pointer lives in slot 0). Every path that
// builds an array must initialise its slots: holes must read as holes, and no
// stale element or pointer may show through, across minor and major GCs.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

const kept = [];
function check(a, n, tag) {
  same(a.length, n, `${tag} length`);
  for (let k = 0; k < n; k++) same(a[k], k * 3 + 1, `${tag}[${k}]`);
}
function holes(a, n, tag) {
  same(a.length, n, `${tag} length`);
  for (let k = 0; k < n; k++) {
    same(k in a, false, `${tag} hole ${k}`);
    same(a[k], undefined, `${tag} hole read ${k}`);
  }
}

for (let round = 0; round < 400; round++) {
  // garbage of every cached size, then fresh arrays that may reuse it
  for (let g = 0; g < 200; g++) {
    const junk = [];
    for (let k = 0; k < (g % 40); k++) junk.push({ g, k });
  }
  const n = round % 37;
  const pushed = [];
  for (let k = 0; k < n; k++) pushed.push(k * 3 + 1);
  check(pushed, n, `push ${n}`);
  const sparse = new Array(n);
  holes(sparse, n, `new Array(${n})`);
  const lit = [1, 4, 7, 10, 13, 16, 19, 22];
  check(lit, 8, 'literal 8');
  const holey = [, , 7];
  same(0 in holey, false, 'literal hole');
  same(holey[2], 7, 'literal after hole');
  check(pushed.slice(), n, `slice ${n}`);
  check(pushed.map((v) => v), n, `map ${n}`);
  check(Array.from(pushed), n, `from ${n}`);
  check([].concat(pushed), n, `concat ${n}`);
  const spliced = pushed.slice();
  spliced.splice(0, 0);
  check(spliced, n, `splice ${n}`);
  const grown = [];
  grown[n] = 'end';
  holes(grown.slice(0, n), n, `grown holes ${n}`);
  same(grown[n], 'end', `grown end ${n}`);
  if (round % 10 === 0) kept.push(pushed, sparse, grown);
}
for (let i = 0; i < kept.length; i += 3) {
  const n = (i / 3 * 10) % 37;
  check(kept[i], n, `kept push ${n}`);
  holes(kept[i + 1], n, `kept sparse ${n}`);
}
console.log('PASS reused array storage starts clean');
