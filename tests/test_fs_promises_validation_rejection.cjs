const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');

async function expectRejectedPromise(name, call) {
  let result;
  try {
    result = call();
  } catch (error) {
    assert.fail(`${name} threw synchronously: ${error && error.stack ? error.stack : error}`);
  }

  assert.strictEqual(result instanceof Promise, true, `${name} must return a Promise`);

  let rejection;
  try {
    await result;
  } catch (error) {
    rejection = error;
  }

  assert.strictEqual(rejection && rejection.name, 'TypeError', `${name} must reject with TypeError`);
}

async function main() {
  await expectRejectedPromise('fs/promises.readFile()', () => fsp.readFile());
  await expectRejectedPromise('fs/promises.writeFile()', () => fsp.writeFile());
  await expectRejectedPromise('fs/promises.stat()', () => fsp.stat());
  await expectRejectedPromise('fs.promises.symlink()', () => fs.promises.symlink());

  const adapterProbe = (() => {
    try {
      return fsp.readFile().catch((error) => error);
    } catch (error) {
      return error;
    }
  })();
  assert.strictEqual(adapterProbe instanceof Promise, true);
  assert.strictEqual((await adapterProbe).name, 'TypeError');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
