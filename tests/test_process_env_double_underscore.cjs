function assert(condition, message) {
  if (!condition) throw new Error(message);
}

process.env.__ANT_ENV_DOUBLE = 'from-js';

assert(process.env.__ANT_ENV_DOUBLE === 'from-js', 'process.env getter should read __* assignments');

const keys = Object.keys(process.env);
assert(keys.includes('__ANT_ENV_DOUBLE'), 'Object.keys(process.env) should include __* assignments');

const obj = process.env.toObject();
assert(obj.__ANT_ENV_DOUBLE === 'from-js', 'process.env.toObject() should include __* assignments');

const envString = process.env.toString();
assert(
  envString.split('\n').includes('__ANT_ENV_DOUBLE=from-js'),
  'process.env.toString() should include __* assignments'
);

console.log('process-env-double-underscore:ok');
