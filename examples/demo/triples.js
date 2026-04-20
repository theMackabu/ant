const tripleCache = new Map();
const gcdCache = new Map();

function gcd(a, b) {
  const key = (a << 16) | b;
  if (gcdCache.has(key)) return gcdCache.get(key);
  const result = b === 0 ? a : gcd(b, a % b);
  gcdCache.set(key, result);
  return result;
}

function pushMultiples(a, b, c, k, limit, acc) {
  if (k * c > limit) return acc;
  acc.push([k * a, k * b, k * c]);
  return pushMultiples(a, b, c, k + 1, limit, acc);
}

function generate(m, n, limit, acc) {
  if (m * m + 1 > limit) return acc;
  if (n >= m) return generate(m + 1, 1, limit, acc);

  const c = m * m + n * n;
  if (c > limit) return generate(m + 1, 1, limit, acc);

  if (((m ^ n) & 1) === 1 && gcd(m, n) === 1) {
    const p = m * m - n * n;
    const q = 2 * m * n;
    const [a, b] = p < q ? [p, q] : [q, p];
    pushMultiples(a, b, c, 1, limit, acc);
  }

  return generate(m, n + 1, limit, acc);
}

function pythagoreanTriples(limit) {
  if (tripleCache.has(limit)) return tripleCache.get(limit);
  const triples = generate(2, 1, limit, []);
  triples.sort((x, y) => x[2] - y[2] || x[0] - y[0]);
  tripleCache.set(limit, triples);
  return triples;
}

const start = performance.now();
const result = pythagoreanTriples(200);
const end = performance.now();

console.log(`Found ${result.length} Pythagorean triples with c ≤ 200`);
console.log(result);

console.log(`Time: ${(end - start).toFixed(4)} ms (${((end - start) * 1000).toFixed(2)} µs)`);
