// Test void operator with promises and async functions
Ant.println('=== Void Operator Tests ===');

// Test 1: void with async function (fire and forget)
Ant.println('\nTest 1: void with async function');
async function asyncTask() {
  Ant.println('Async task executing');
  return 'task result';
}

// Using void indicates we don't care about the result/promise
void asyncTask();
Ant.println('After void async call');

// Test 2: void with Promise.resolve
Ant.println('\nTest 2: void with Promise.resolve');
void Promise.resolve(42).then(v => Ant.println('Promise resolved with: ' + v));
Ant.println('After void Promise.resolve');

// Test 3: void with regular function call
Ant.println('\nTest 3: void with regular function');
function regularFunc() {
  Ant.println('Regular function called');
  return 'regular result';
}
const result = void regularFunc();
Ant.println('Result of void is: ' + result);

// Test 4: void in expression
Ant.println('\nTest 4: void in expression');
const x = 10;
const y = void x;
Ant.println('void x where x=10 is: ' + y);

// Test 5: void with multiple expressions
Ant.println('\nTest 5: void prevents await');
async function noAwait() {
  // void means we explicitly don't want to await this
  void Promise.resolve('not awaited');
  Ant.println('Continued without awaiting');
  return 'done';
}
noAwait().then(v => Ant.println('noAwait returned: ' + v));

// Test 6: void vs await comparison
Ant.println('\nTest 6: void vs await comparison');
async function withAwait() {
  const result = await Promise.resolve('awaited value');
  Ant.println('With await: ' + result);
  return result;
}

async function withVoid() {
  void Promise.resolve('void value');
  Ant.println('With void: undefined (not waiting)');
  return 'immediate return';
}

withAwait().then(v => Ant.println('withAwait result: ' + v));
withVoid().then(v => Ant.println('withVoid result: ' + v));

// Test 7: void with chained promise (fire and forget)
Ant.println('\nTest 7: void with promise chain');
void Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => Ant.println('Chained promise result: ' + v));
Ant.println('Promise chain started but not awaited');

// Test 8: void return value
Ant.println('\nTest 8: void always returns undefined');
function returnVoid() {
  return void 100;
}
const voidResult = returnVoid();
Ant.println('Function returning void 100: ' + voidResult);

// Test 9: void with function expression
Ant.println('\nTest 9: void with function expression');
void function() {
  Ant.println('IIFE with void executed');
}();

// Test 10: void indicating intentional non-handling
Ant.println('\nTest 10: void for intentional non-handling');
async function backgroundTask() {
  return await Promise.resolve('background work done');
}

async function mainTask() {
  // void explicitly shows we don't want to wait for backgroundTask
  void backgroundTask();
  Ant.println('Main task continues immediately');
  return 'main done';
}

mainTask().then(v => Ant.println('mainTask result: ' + v));

Ant.println('\n=== Synchronous code finished ===');
