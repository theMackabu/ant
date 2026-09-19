// A thrown undefined is an abrupt completion, not an absent stringify error.
function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(action, reason, label) {
  let caught = false;
  try { action(); }
  catch (error) {
    caught = true;
    assert(error === reason, label + ': original throw changed');
  }
  assert(caught, label + ': throw was swallowed');
}

let queued = false;
queueMicrotask(() => { queued = true; });
for (const reason of [undefined, null, new Error('stringify error')]) {
  let calls = 0;
  expectThrow(() => JSON.stringify({ a: 1, b: 2 }, (key, value) => {
    calls++;
    if (key === 'a') throw reason;
    return value;
  }), reason, 'replacer');
  assert(calls === 2, 'replacer continued after throw');

  expectThrow(() => JSON.stringify({ toJSON() { throw reason; } }), reason, 'toJSON');
  expectThrow(() => JSON.stringify({ get a() { throw reason; } }), reason, 'property getter');
  const array = [1];
  Object.defineProperty(array, 0, { get() { throw reason; } });
  expectThrow(() => JSON.stringify(array), reason, 'array getter');

  const space = new Number(2);
  space[Symbol.toPrimitive] = () => { throw reason; };
  expectThrow(() => JSON.stringify({ a: 1 }, null, space), reason, 'indent coercion');

  let lengthReads = 0;
  const replacerProxy = new Proxy(['a'], {
    get(target, key) {
      if (key === 'length') { lengthReads++; throw reason; }
      return target[key];
    }
  });
  expectThrow(() => JSON.stringify({ a: 1 }, replacerProxy), reason, 'replacer length getter');
  assert(lengthReads === 1, 'replacer length getter repeated');

  for (const input of [{ a: 1, b: 2 }, { 0: 1, a: 2 }]) {
    let itemReads = 0;
    const propertyList = ['a'];
    Object.defineProperty(propertyList, 0, { get() { itemReads++; throw reason; } });
    expectThrow(() => JSON.stringify(input, propertyList), reason, 'replacer item getter');
    assert(itemReads === 1, 'replacer item getter continued after throw');
  }
}
assert(JSON.stringify({ a: 1, b: undefined }) === '{"a":1}', 'ordinary undefined changed');
setTimeout(() => {
  assert(queued, 'queued work was dropped');
  console.log('PASS');
}, 0);
