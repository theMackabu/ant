const now = () =>
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now();

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

const routeCorpus = new Array(2000)
  .fill(0)
  .map((_, i) => `/api/v1/users/${i}/posts/${i % 17}?limit=50&offset=${i}`)
  .join('\n');

const unicodeCorpus = new Array(2500)
  .fill('alpha beta gamma delta user_123 route_456 JSON42 token99')
  .join(' ');

const routeSource =
  '^/api/v(?<version>[0-9]+)/users/(?<user>[0-9]+)/posts/(?<post>[0-9]+)(?:\\?(?<query>.*))?$';

const routeRe = new RegExp(routeSource, 'gm');
const tokenRe = /\b[a-z_]+[0-9]*\b/g;
const splitRe = /[A-Za-z_][A-Za-z0-9_]{0,31}/g;

console.log('=== Regex Benchmark ===');
console.log(`route corpus bytes: ${routeCorpus.length}`);
console.log(`unicode corpus bytes: ${unicodeCorpus.length}`);
console.log('');

bench('compile route regexp x5000', 5000, () => new RegExp(routeSource, 'gm'));

bench('route matches x400', 400, () => {
  routeRe.lastIndex = 0;
  let count = 0;
  while (routeRe.exec(routeCorpus)) count++;
  return count;
});

bench('token scan x500', 500, () => {
  tokenRe.lastIndex = 0;
  let count = 0;
  while (tokenRe.exec(unicodeCorpus)) count++;
  return count;
});

bench('identifier split x500', 500, () => {
  splitRe.lastIndex = 0;
  let count = 0;
  while (splitRe.exec(unicodeCorpus)) count++;
  return count;
});
