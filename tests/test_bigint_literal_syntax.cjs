const assert = require('node:assert');
const vm = require('node:vm');

function parses(src) {
  try { new vm.Script(src); return true } catch (e) {
    if (e instanceof SyntaxError) return false;
    throw e;
  }
}

const invalid = [
  "x = 01n",
  "x = 1.5n",
  "x = 1e3n",
];

const valid = [
  "x = 0n",
  "x = 10n",
  "x = 0xEn",
  "x = 0x1en",
  "x = 0XFFn",
  "x = 0b1n",
  "x = 0o7n",
  "x = 1_000n",
];

for (const src of valid) assert.strictEqual(parses(src), true, 'should parse: ' + JSON.stringify(src));
for (const src of invalid) assert.strictEqual(parses(src), false, 'should be a SyntaxError: ' + JSON.stringify(src));

console.log('ok');
