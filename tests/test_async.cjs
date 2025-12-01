// Test async/await functionality
Ant.println('=== Async/Await Tests ===');

// Test 1: Basic async function
Ant.println('\nTest 1: Basic async function');
async function basicAsync() {
  return 42;
}

basicAsync().then(v => {
  Ant.println('Basic async returned: ' + v);
});

// Test 2: Async arrow function
Ant.println('\nTest 2: Async arrow function');
const arrowAsync = async () => {
  return 'arrow result';
};

arrowAsync().then(v => {
  Ant.println('Arrow async returned: ' + v);
});

// Test 3: Async arrow function with single parameter
Ant.println('\nTest 3: Async arrow with param');
const singleParamAsync = async x => {
  return x * 2;
};

singleParamAsync(21).then(v => {
  Ant.println('Single param async returned: ' + v);
});

// Test 4: Async arrow function with multiple parameters
Ant.println('\nTest 4: Async arrow with multiple params');
const multiParamAsync = async (a, b) => {
  return a + b;
};

multiParamAsync(10, 32).then(v => {
  Ant.println('Multi param async returned: ' + v);
});

// Test 5: Async function with Promise
Ant.println('\nTest 5: Async function returning Promise');
async function withPromise() {
  return Promise.resolve('promise from async');
}

withPromise().then(v => {
  Ant.println('Async with promise: ' + v);
});

// Test 6: Async function with setTimeout
Ant.println('\nTest 6: Async function with setTimeout');
async function withTimeout() {
  Ant.setTimeout(() => {
    Ant.println('Timeout inside async executed');
  }, 100);
  return 'timeout scheduled';
}

withTimeout().then(v => {
  Ant.println('Async with timeout returned: ' + v);
});

// Test 7: Async function with queueMicrotask
Ant.println('\nTest 7: Async function with queueMicrotask');
async function withMicrotask() {
  Ant.queueMicrotask(() => {
    Ant.println('Microtask inside async executed');
  });
  return 'microtask queued';
}

withMicrotask().then(v => {
  Ant.println('Async with microtask returned: ' + v);
});

// Test 8: Async functions must return primitives for now (Promise chaining limitation)
Ant.println('\nTest 8: Async returning values');
async function chain1() {
  return 5;
}

chain1().then(v => {
  Ant.println('Chained async result: ' + (v * 2));
});

// Test 9: Async function as callback
Ant.println('\nTest 9: Async function as callback');
function executor(fn) {
  fn().then(v => {
    Ant.println('Callback async result: ' + v);
  });
}

executor(async () => {
  return 'callback value';
});

// Test 10: Multiple promise chains
Ant.println('\nTest 10: Multiple promise chains');
Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => {
    Ant.println('Multiple chains result: ' + v);
  });

// Test 11: Async function expression
Ant.println('\nTest 11: Async function expression');
const asyncExpr = async function() {
  return 'expression';
};

asyncExpr().then(v => {
  Ant.println('Async expression result: ' + v);
});

// Test 12: Async function with conditional
Ant.println('\nTest 12: Async function with conditional');
async function conditional(flag) {
  if (flag) {
    return 'true branch';
  }
  return 'false branch';
}

conditional(true).then(v => {
  Ant.println('Conditional async (true): ' + v);
});

conditional(false).then(v => {
  Ant.println('Conditional async (false): ' + v);
});

// Test 13: Async with Promise.resolve
Ant.println('\nTest 13: Async with Promise.resolve');
async function withResolve() {
  return Promise.resolve('resolved value');
}

withResolve().then(v => {
  Ant.println('Async with resolve result: ' + v);
});

// Test 14: Async with object method
Ant.println('\nTest 14: Async object method');
const obj = {
  value: 100,
  asyncMethod: async function() {
    return this.value;
  }
};

obj.asyncMethod().then(v => {
  Ant.println('Async object method: ' + v);
});

// Test 15: Async arrow in object
Ant.println('\nTest 15: Async arrow in object');
const obj2 = {
  value: 200,
  asyncArrow: async () => {
    return 'arrow in object';
  }
};

obj2.asyncArrow().then(v => {
  Ant.println('Async arrow in object: ' + v);
});

Ant.println('\n=== Synchronous code finished ===');
