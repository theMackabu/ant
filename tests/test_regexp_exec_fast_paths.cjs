function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

const routeRe =
  /^\/api\/v(?<version>[0-9]+)\/users\/(?<user>[0-9]+)\/posts\/(?<post>[0-9]+)(?:\?(?<query>.*))?$/;
const routeMatch = routeRe.exec('/api/v3/users/42/posts/9?limit=10');

assert(routeMatch !== null, 'expected route regexp to match');
assert(routeMatch[0] === '/api/v3/users/42/posts/9?limit=10', 'full match mismatch');

const groups1 = routeMatch.groups;
const groups2 = routeMatch.groups;
assert(groups1 === groups2, 'groups getter should cache the created object');
assert(groups1.version === '3', 'named group version mismatch');
assert(groups1.user === '42', 'named group user mismatch');
assert(groups1.post === '9', 'named group post mismatch');
assert(groups1.query === 'limit=10', 'named group query mismatch');

const wordRe = /\b[a-z]+\b/g;
const words = 'alpha beta gamma';
let count = 0;
let lastMatch = '';

while (wordRe.exec(words)) {
  count++;
  lastMatch = RegExp.lastMatch;
}

assert(count === 3, 'truthy exec loop should count all matches');
assert(lastMatch === 'gamma', 'RegExp.lastMatch should track the final successful exec');
assert(wordRe.lastIndex === 0, 'global exec loop should reset lastIndex after the final miss');

const order = [];
let customCalls = 0;
const customExec = {
  get exec() {
    order.push('get');
    return function (value) {
      order.push('call:' + value);
      return customCalls++ === 0 ? { ok: true } : null;
    };
  }
};

function nextCustomArg() {
  order.push('arg');
  return 'payload';
}

let customCount = 0;
while (customExec.exec(nextCustomArg())) {
  customCount++;
}

assert(customCount === 1, 'custom exec truthiness loop should still use fallback call semantics');
assert(
  order.join(',') === 'get,arg,call:payload,get,arg,call:payload',
  'custom exec truthiness lowering should preserve getter/arg/call order'
);

const testGets = [];
const testProxy = new Proxy(
  {
    exec() {
      return null;
    }
  },
  {
    get(target, key) {
      testGets.push(key);
      return target[key];
    }
  }
);

assert(RegExp.prototype.test.call(testProxy) === false, 'proxy-backed test should return false');
assert(testGets.join(',') === 'exec', 'RegExp.prototype.test should only Get("exec") once');

const flagKeys = [];
const flagProxy = new Proxy(
  {},
  {
    get(target, key) {
      flagKeys.push(key);
      return target[key];
    }
  }
);

Object.getOwnPropertyDescriptor(RegExp.prototype, 'flags').get.call(flagProxy);

const expectedFlagKeys = [];
if ('hasIndices' in RegExp.prototype) expectedFlagKeys.push('hasIndices');
if ('global' in RegExp.prototype) expectedFlagKeys.push('global');
if ('ignoreCase' in RegExp.prototype) expectedFlagKeys.push('ignoreCase');
if ('multiline' in RegExp.prototype) expectedFlagKeys.push('multiline');
if ('dotAll' in RegExp.prototype) expectedFlagKeys.push('dotAll');
if ('unicode' in RegExp.prototype) expectedFlagKeys.push('unicode');
if ('unicodeSets' in RegExp.prototype) expectedFlagKeys.push('unicodeSets');
if ('sticky' in RegExp.prototype) expectedFlagKeys.push('sticky');

assert(
  flagKeys.join(',') === expectedFlagKeys.join(','),
  'RegExp.prototype.flags should read observable flag properties in spec order'
);

console.log('regex exec fast path semantics ok');
