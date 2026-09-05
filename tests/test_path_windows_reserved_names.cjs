const assert = require('node:assert');
const { win32 } = require('node:path');

// Node upstream b0a4f162: a missing colon must not truncate the name
// before checking whether it is a Windows reserved device.
for (const name of ['CONx', 'NULl', 'NULs', 'LPT1x', 'PRNzzz', 'CON']) {
  assert.strictEqual(win32.normalize(name), name);
}
assert.strictEqual(win32.normalize('CON:'), '.\\CON:.');
console.log('Windows reserved-name normalization ok');
