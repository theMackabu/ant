const assert = require('node:assert');
const bytes = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0,
  2, 12, 1, 3, 101, 110, 118, 4, 102, 97, 105, 108, 0, 0,
  3, 2, 1, 0, 7, 7, 1, 3, 114, 117, 110, 0, 1,
  10, 6, 1, 4, 0, 16, 0, 11,
]);
const module = new WebAssembly.Module(bytes);

(async () => {
  for (const reason of [undefined, null, new Error('import getter')]) {
    for (const imports of [
      { get env() { throw reason; } },
      { env: { get fail() { throw reason; } } },
    ]) {
      let caught = false;
      try {
        new WebAssembly.Instance(module, imports);
      } catch (error) {
        caught = true;
        assert.strictEqual(error, reason);
      }
      assert.ok(caught, 'instance import getter must throw');

      for (const source of [module, bytes]) {
        const promise = WebAssembly.instantiate(source, imports);
        assert.ok(promise instanceof Promise, 'instantiate must return a Promise');
        caught = false;
        try {
          await promise;
        } catch (error) {
          caught = true;
          assert.strictEqual(error, reason);
        }
        assert.ok(caught, 'instantiate import getter must reject');
      }
    }
  }
  const instance = await WebAssembly.instantiate(module, { env: { fail() {} } });
  instance.exports.run();
  await new Promise(resolve => setTimeout(resolve, 0));
  console.log('Wasm error boundaries ok');
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
