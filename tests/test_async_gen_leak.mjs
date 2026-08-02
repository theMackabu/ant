async function* ag() {
  for (let i = 0; i < 5000; i++) {
    await Promise.resolve();
    yield i;
  }
}
let total = 0,
  rounds = 0;
for (let r = 0; r < 40; r++) {
  let s = 0;
  for await (const v of ag()) s += v;
  total += s;
  rounds++;
}
console.log('rounds:', rounds, 'total:', total, 'rss MB:', Math.round(process.memoryUsage().rss / 1048576));
