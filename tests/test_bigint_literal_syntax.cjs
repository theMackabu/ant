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

for (const src of invalid) assert.strictEqual(parses(src), false, 'should be a SyntaxError: ' + JSON.stringify(src));

console.log('ok');
