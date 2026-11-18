const assert = require('node:assert');

const outcome = (make) => {
  try {
    return { value: make() };
  } catch (error) {
    return { error: error.name };
  }
};

const worksOrRangeError = (label, make, check) => {
  const result = outcome(make);
  if (result.error) assert.strictEqual(result.error, 'RangeError', label);
  else if (check) assert.ok(check(result.value), label);
};

{
  let chained = () => 0;
  for (let i = 0; i < 1000000; i++) { const inner = chained; chained = () => inner() + 1; }
  let junk = [];
  for (let i = 0; i < 300000; i++) junk = [junk.length, i];
  worksOrRangeError('deep closure call', () => chained());
}

console.log('ok');
