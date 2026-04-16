const { inspect } = require('node:util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

const longValue = '1234567890123456789012345678901234567890123456789012345678901234567890';
const promise = Promise.resolve({ a: longValue, b: 2 });
const rendered = inspect(promise);

assert(rendered.includes(`a: '${longValue}'`), `expected full promise payload in inspect output, got: ${rendered}`);
assert(rendered.includes('b: 2'), `expected secondary property in inspect output, got: ${rendered}`);
assert(rendered.includes('Symbol(async_id):'), `expected promise metadata in inspect output, got: ${rendered}`);

console.log('PASS');
