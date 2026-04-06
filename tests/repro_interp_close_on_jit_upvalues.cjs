// Repro for mixed-mode upvalue corruption:
// a hot child closure bails out to the interpreter, then executes OP_CLOSE_UPVAL
// while its hot parent frame still has open captured locals in JIT storage.

function makeHandle(prefix) {
  return function handle(seed, triggerBailout) {
    let count = seed + 1;
    const box = { prefix, seed };

    function sibling() {
      return prefix.length + count + box.seed;
    }

    function next(value, bail) {
      let total = prefix.length + count;

      for (let i = 0; i < 1; i++) {
        let captured = 7;
        const readCaptured = () => captured;
        total += readCaptured();

        // Warm numerically, then force a bailout before the loop scope closes.
        if (bail) total = total + value;
      }

      return total;
    }

    const first = next(triggerBailout ? '!' : 1, triggerBailout);

    // If the child interpreter close path corrupts this upvalue, sibling()
    // will stop observing the live parent slot after we mutate count.
    count += 10;

    return {
      first,
      sibling: sibling(),
      count,
    };
  };
}

const handle = makeHandle('root');

for (let i = 0; i < 250; i++) {
  const got = handle(i, false);
  const want = {
    first: 4 + (i + 1) + 7,
    sibling: 4 + (i + 11) + i,
    count: i + 11,
  };

  if (
    got.first !== want.first ||
    got.sibling !== want.sibling ||
    got.count !== want.count
  ) {
    throw new Error(
      `warmup mismatch at ${i}: expected ${JSON.stringify(want)}, got ${JSON.stringify(got)}`
    );
  }
}

const got = handle(10, true);
const want = {
  first: '22!',
  sibling: 35,
  count: 21,
};

if (
  got.first !== want.first ||
  got.sibling !== want.sibling ||
  got.count !== want.count
) {
  throw new Error(`expected ${JSON.stringify(want)}, got ${JSON.stringify(got)}`);
}

console.log('OK: bailout + interpreter close preserved parent JIT upvalues', JSON.stringify(got));
