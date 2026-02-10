const DURATION = 5_000;
const origin = performance.now();
let count = 0;

function iter() {
  count++;
  if (performance.now() - origin < DURATION) setImmediate(iter);
}

iter();

process.once('beforeExit', () => {
  const elapsed = performance.now() - origin;
  const rate = count / (elapsed / 1000);

  let formatted;
  if (rate >= 1_000_000) formatted = (rate / 1_000_000).toFixed(2) + 'M';
  else if (rate >= 1_000) formatted = (rate / 1_000).toFixed(1) + 'K';
  else formatted = String(Math.round(rate));

  console.log(`\x1b[1;36m${formatted} event loop iterations/sec\x1b[0m \x1b[2m(${count.toLocaleString()} in ${(elapsed / 1000).toFixed(1)}s)\x1b[0m`);
});
