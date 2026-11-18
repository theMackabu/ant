const assert = require('node:assert');

const ROUNDS = 20000;

function captured(n) {
  const readers = [];
  let sum = 0;
  for (let i = 0; i < n; i++) {
    let local = i;
    try { try { readers.length = 0; readers.push(() => local); throw local; } finally { local++; } } catch (e) { sum += readers[0]() - e; }
  }
  return sum;
}
assert.strictEqual(captured(ROUNDS), ROUNDS);

function plain(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    try { throw i; } catch (e) { sum += e === i ? 1 : 0; }
    try { throw { i }; } catch ({ i: got }) { sum += got === i ? 1 : 0; }
    try { throw undefined; } catch (e) { sum += e === undefined ? 1 : 0; }
    try { throw null; } catch { sum++; }
  }
  return sum;
}
assert.strictEqual(plain(ROUNDS), 4 * ROUNDS);

function errorIdentity(n) {
  const error = new Error('kept');
  const stack = error.stack;
  let same = 0;
  for (let i = 0; i < n; i++) {
    try { throw error; } catch (e) { if (e === error && e.stack === stack) same++; }
  }
  return same;
}
assert.strictEqual(errorIdentity(ROUNDS), ROUNDS);

console.log('ok');
