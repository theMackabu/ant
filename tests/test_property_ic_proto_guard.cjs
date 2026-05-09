const assert = require('assert');

const protoA = { marker: 11 };
const protoB = { marker: 29 };

function make(proto) {
  return { __proto__: proto, own: 1 };
}

function marker(obj) {
  return obj.marker;
}

for (let i = 0; i < 64; i++) {
  assert.strictEqual(marker(make(protoA)), 11);
}

assert.strictEqual(marker(make(protoB)), 29);

console.log('property-ic-proto-guard:ok');
