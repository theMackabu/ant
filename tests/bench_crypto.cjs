const now = () =>
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now();

const { createHash, randomBytes } =
  typeof require === 'function' ? require('crypto') : Ant.require('crypto');

function bench(name, rounds, fn) {
  fn();
  const start = now();
  let result;
  for (let i = 0; i < rounds; i++) result = fn();
  const elapsed = now() - start;
  console.log(
    `${name}: ${elapsed.toFixed(2)}ms total, ${(elapsed / rounds).toFixed(4)}ms/round`
  );
  return result;
}

const small = 'The quick brown fox jumps over the lazy dog';
const medium = '0123456789abcdef'.repeat(4096);
const large = 'abcdef0123456789'.repeat(65536);

console.log('=== Crypto Benchmark ===');
console.log(`small bytes: ${small.length}`);
console.log(`medium bytes: ${medium.length}`);
console.log(`large bytes: ${large.length}`);
console.log('');

bench('sha256 small x20000', 20000, () =>
  createHash('sha256').update(small).digest('hex')
);

bench('sha256 medium x1500', 1500, () =>
  createHash('sha256').update(medium).digest('hex')
);

bench('sha256 large x120', 120, () =>
  createHash('sha256').update(large).digest('hex')
);

bench('sha512 medium x1000', 1000, () =>
  createHash('sha512').update(medium).digest('hex')
);

bench('randomBytes 4k x4000', 4000, () => randomBytes(4096));
