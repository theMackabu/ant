// Test async/await functionality
console.log('=== Async/Await Tests ===');

// Test 1: Basic async function
console.log('\nTest 1: Basic async function');
async function basicAsync() {
  return 42;
}

basicAsync().then(v => {
  console.log('Basic async returned: ' + v);
});

// Test 2: Async arrow function
console.log('\nTest 2: Async arrow function');
const arrowAsync = async () => {
  return 'arrow result';
};

arrowAsync().then(v => {
  console.log('Arrow async returned: ' + v);
});

// Test 3: Async arrow function with single parameter
console.log('\nTest 3: Async arrow with param');
const singleParamAsync = async x => {
  return x * 2;
};

singleParamAsync(21).then(v => {
  console.log('Single param async returned: ' + v);
});

// Test 4: Async arrow function with multiple parameters
console.log('\nTest 4: Async arrow with multiple params');
const multiParamAsync = async (a, b) => {
  return a + b;
};

multiParamAsync(10, 32).then(v => {
  console.log('Multi param async returned: ' + v);
});

// Test 5: Async function with Promise
console.log('\nTest 5: Async function returning Promise');
async function withPromise() {
  return Promise.resolve('promise from async');
}

withPromise().then(v => {
  console.log('Async with promise: ' + v);
});

// Test 6: Async function with setTimeout
console.log('\nTest 6: Async function with setTimeout');
async function withTimeout() {
  Ant.setTimeout(() => {
    console.log('Timeout inside async executed');
  }, 100);
  return 'timeout scheduled';
}

withTimeout().then(v => {
  console.log('Async with timeout returned: ' + v);
});

// Test 7: Async function with queueMicrotask
console.log('\nTest 7: Async function with queueMicrotask');
async function withMicrotask() {
  Ant.queueMicrotask(() => {
    console.log('Microtask inside async executed');
  });
  return 'microtask queued';
}

withMicrotask().then(v => {
  console.log('Async with microtask returned: ' + v);
});

// Test 8: Async functions must return primitives for now (Promise chaining limitation)
console.log('\nTest 8: Async returning values');
async function chain1() {
  return 5;
}

chain1().then(v => {
  console.log('Chained async result: ' + (v * 2));
});

// Test 9: Async function as callback
console.log('\nTest 9: Async function as callback');
function executor(fn) {
  fn().then(v => {
    console.log('Callback async result: ' + v);
  });
}

executor(async () => {
  return 'callback value';
});

// Test 10: Multiple promise chains
console.log('\nTest 10: Multiple promise chains');
Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => {
    console.log('Multiple chains result: ' + v);
  });

// Test 11: Async function expression
console.log('\nTest 11: Async function expression');
const asyncExpr = async function() {
  return 'expression';
};

asyncExpr().then(v => {
  console.log('Async expression result: ' + v);
});

// Test 12: Async function with conditional
console.log('\nTest 12: Async function with conditional');
async function conditional(flag) {
  if (flag) {
    return 'true branch';
  }
  return 'false branch';
}

conditional(true).then(v => {
  console.log('Conditional async (true): ' + v);
});

conditional(false).then(v => {
  console.log('Conditional async (false): ' + v);
});

// Test 13: Async with Promise.resolve
console.log('\nTest 13: Async with Promise.resolve');
async function withResolve() {
  return Promise.resolve('resolved value');
}

withResolve().then(v => {
  console.log('Async with resolve result: ' + v);
});

// Test 14: Async with object method
console.log('\nTest 14: Async object method');
const obj = {
  value: 100,
  asyncMethod: async function() {
    return this.value;
  }
};

obj.asyncMethod().then(v => {
  console.log('Async object method: ' + v);
});

// Test 15: Async arrow in object
console.log('\nTest 15: Async arrow in object');
const obj2 = {
  value: 200,
  asyncArrow: async () => {
    return 'arrow in object';
  }
};

obj2.asyncArrow().then(v => {
  console.log('Async arrow in object: ' + v);
});

console.log('\n=== Synchronous code finished ===');
