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

function mixedParameters(count, value, missing) {
  let sum = 0;
  while (count-- > 0) sum += value;
  return [sum, missing, count];
}

function capturedWrite(count, value) {
  function change() { value++; }
  let sum = 0;
  while (count-- > 0) {
    change();
    sum += value;
  }
  return sum;
}

function defaultAndInvariant(count, value = 3, multiplier = 2) {
  let sum = 0;
  while (count-- > 0) sum += value * multiplier;
  return sum;
}

for (let i = 0; i < 1200; i++) {
  assert.deepStrictEqual(mixedParameters(40, 3), [120, undefined, -1]);
  assert.deepStrictEqual(mixedParameters(40, 4, 'present'), [160, 'present', -1]);
  assert.strictEqual(capturedWrite(40, 2), 900);
  assert.strictEqual(defaultAndInvariant(40), 240);
  assert.strictEqual(defaultAndInvariant(40, 4, 3), 480);
}

assert.deepStrictEqual(mixedParameters(8000, 5), [40000, undefined, -1]);

function tailAssigned(count, value) {
  value = value === undefined ? 0 : value;
  if (count === 0) return value;
  return tailAssigned(count - 1, value + 3);
}

function tailMissing(count, value) {
  value = value === undefined ? 1 : value + 1;
  if (count === 0) return value;
  if (count === 3) return tailMissing(count - 1);
  return tailMissing(count - 1, value);
}

assert.strictEqual(tailAssigned(2000), 6000);
for (let i = 0; i < 1200; i++) assert.strictEqual(tailMissing(7, 10), 3);
