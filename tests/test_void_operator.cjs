// Test void operator with promises and async functions
console.log('=== Void Operator Tests ===');

// Test 1: void with async function (fire and forget)
console.log('\nTest 1: void with async function');
async function asyncTask() {
  console.log('Async task executing');
  return 'task result';
}

// Using void indicates we don't care about the result/promise
void asyncTask();
console.log('After void async call');

// Test 2: void with Promise.resolve
console.log('\nTest 2: void with Promise.resolve');
void Promise.resolve(42).then(v => console.log('Promise resolved with: ' + v));
console.log('After void Promise.resolve');

// Test 3: void with regular function call
console.log('\nTest 3: void with regular function');
function regularFunc() {
  console.log('Regular function called');
  return 'regular result';
}
const result = void regularFunc();
console.log('Result of void is: ' + result);

// Test 4: void in expression
console.log('\nTest 4: void in expression');
const x = 10;
const y = void x;
console.log('void x where x=10 is: ' + y);

// Test 5: void with multiple expressions
console.log('\nTest 5: void prevents await');
async function noAwait() {
  // void means we explicitly don't want to await this
  void Promise.resolve('not awaited');
  console.log('Continued without awaiting');
  return 'done';
}
noAwait().then(v => console.log('noAwait returned: ' + v));

// Test 6: void vs await comparison
console.log('\nTest 6: void vs await comparison');
async function withAwait() {
  const result = await Promise.resolve('awaited value');
  console.log('With await: ' + result);
  return result;
}

async function withVoid() {
  void Promise.resolve('void value');
  console.log('With void: undefined (not waiting)');
  return 'immediate return';
}

withAwait().then(v => console.log('withAwait result: ' + v));
withVoid().then(v => console.log('withVoid result: ' + v));

// Test 7: void with chained promise (fire and forget)
console.log('\nTest 7: void with promise chain');
void Promise.resolve(1)
  .then(v => v + 1)
  .then(v => v + 1)
  .then(v => console.log('Chained promise result: ' + v));
console.log('Promise chain started but not awaited');

// Test 8: void return value
console.log('\nTest 8: void always returns undefined');
function returnVoid() {
  return void 100;
}
const voidResult = returnVoid();
console.log('Function returning void 100: ' + voidResult);

// Test 9: void with function expression
console.log('\nTest 9: void with function expression');
void function() {
  console.log('IIFE with void executed');
}();

// Test 10: void indicating intentional non-handling
console.log('\nTest 10: void for intentional non-handling');
async function backgroundTask() {
  return await Promise.resolve('background work done');
}

async function mainTask() {
  // void explicitly shows we don't want to wait for backgroundTask
  void backgroundTask();
  console.log('Main task continues immediately');
  return 'main done';
}

mainTask().then(v => console.log('mainTask result: ' + v));

console.log('\n=== Synchronous code finished ===');
