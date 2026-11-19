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

{
  const count = 1000000;
  const weak = new WeakMap();
  let chain = null;
  for (let i = 0; i < count; i++) {
    const inner = chain;
    const payload = { i };
    const key = {};
    weak.set(key, payload);
    chain = () => [inner, payload, key];
  }
  let junk = [];
  for (let i = 0; i < 300000; i++) junk = [junk.length, { i }];
  let seen = 0;
  let wrong = 0;
  for (let link = chain; link; seen++) {
    const [inner, payload, key] = link();
    if (payload.i !== count - 1 - seen || weak.get(key) !== payload) wrong++;
    link = inner;
  }
  assert.strictEqual(seen, count);
  assert.strictEqual(wrong, 0);
}

console.log('ok');
