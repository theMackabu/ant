const assert = require('node:assert');
const { registerHooks } = require('node:module');
function throwsExactly(fn, reason) {
  let caught = false;
  try { fn(); } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'getter error was swallowed');
}
async function main() {
  for (const reason of [undefined, null, new Error('getter')]) {
    let caught = false;
    try { await import('node:os', { with: new Proxy({}, { ownKeys() { throw reason; } }) }); }
    catch (error) { caught = true; assert.strictEqual(error, reason); }
    assert.ok(caught, 'attribute key failure was swallowed');
    const hooks = registerHooks({ resolve() { return { url: 'node:os', get shortCircuit() { throw reason; } }; } });
    try { throwsExactly(() => require('node:os'), reason); } finally { hooks.deregister(); }
    for (const key of [Symbol.asyncIterator, Symbol.iterator, 'next']) {
      const source = {};
      Object.defineProperty(source, key, { get() { throw reason; } });
      throwsExactly(() => AsyncIterator.from(source), reason);
    }
    const source = { get next() { throw reason; } };
    caught = false;
    try { await AsyncIterator.prototype.toArray.call(source); }
    catch (error) { caught = true; assert.strictEqual(error, reason); }
    assert.ok(caught, 'async iterator open failed without original reason');
  }
  console.log('Import and async iterator getter errors ok');
}
main().catch(error => { console.error(error); process.exit(1); });
