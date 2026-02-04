const now = () => Date.now();

function bench(name, fn) {
  const start = now();
  fn();
  const end = now();
  console.log(`${name}: ${end - start}ms`);
}

const digits = '9'.repeat(2000);
const big = BigInt(digits);
const big2 = big * big + 123456789n;

bench('toString(10) x200', () => {
  for (let i = 0; i < 200; i++) {
    big2.toString(10);
  }
});

bench('toString(16) x200', () => {
  for (let i = 0; i < 200; i++) {
    big2.toString(16);
  }
});

bench('toString(2) x40', () => {
  for (let i = 0; i < 40; i++) {
    big2.toString(2);
  }
});
