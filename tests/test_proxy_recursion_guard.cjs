const assert = require('node:assert');

const makeProxyCycle = () => { const object = {}; Object.setPrototypeOf(object, new Proxy(object, {})); return object; };
assert.throws(() => makeProxyCycle().missing, RangeError);
assert.throws(() => { makeProxyCycle().missing = 1; }, RangeError);
assert.throws(() => makeProxyCycle().missing(), RangeError);

console.log('ok');
