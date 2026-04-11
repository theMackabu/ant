function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(fn, label) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  throw new Error(`${label} did not throw`);
}

const dotError = expectThrow(() => {
  const value = undefined;
  return value.call;
}, 'dot access');

assert(
  dotError.message === "Cannot read properties of undefined (reading 'call')",
  `unexpected dot access message: ${dotError.message}`
);

const stringKey = 'digest';
const computedStringError = expectThrow(() => {
  const value = undefined;
  return value[stringKey];
}, 'computed string access');

assert(
  computedStringError.message === "Cannot read properties of undefined (reading 'digest')",
  `unexpected computed string message: ${computedStringError.message}`
);

const numericKey = 0;
const computedNumericError = expectThrow(() => {
  const value = undefined;
  return value[numericKey];
}, 'computed numeric access');

assert(
  computedNumericError.message === "Cannot read properties of undefined (reading '0')",
  `unexpected computed numeric message: ${computedNumericError.message}`
);

console.log('property error message test passed');
