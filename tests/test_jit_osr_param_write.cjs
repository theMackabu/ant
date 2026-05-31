const assert = require('assert');

function readOption(_input, options) {
  options = Object.assign({}, { checkHeader: true }, options);

  let seen = 0;
  for (let i = 0; i < 600; i++) {
    if (!options.checkHeader) throw new Error('lost reassigned parameter');
    seen++;
  }
  return seen;
}

assert.strictEqual(readOption({ length: 1000 }), 600);

function sumAssignedMissing(_input, option) {
  option = 3;
  let sum = 0;
  for (let i = 0; i < 800; i++) sum += option;
  return sum;
}

for (let i = 0; i < 1500; i++) {
  assert.strictEqual(sumAssignedMissing(1), 2400);
}

function capturedAssignedMissing(_input, option) {
  option = 5;
  function readOption() { return option; }
  let sum = 0;
  for (let i = 0; i < 800; i++) sum += readOption();
  return sum;
}

for (let i = 0; i < 1200; i++) {
  assert.strictEqual(capturedAssignedMissing(1), 4000);
}
