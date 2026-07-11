const assert = require('node:assert');

for (const resolve of [
  () => require('ant-test-package-that-does-not-exist'),
  () => require.resolve('ant-test-package-that-does-not-exist'),
]) {
  let error;
  try {
    resolve();
  } catch (caught) {
    error = caught;
  }
  assert(error);
  assert.equal(error.name, 'Error');
  assert.equal(error.code, 'MODULE_NOT_FOUND');
  assert.match(error.message, /ant-test-package-that-does-not-exist/);
}
