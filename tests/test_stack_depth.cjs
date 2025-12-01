// Test that the this stack is properly managed across async boundaries

class StackTest {
  constructor(name) {
    this.name = name;
  }
  
  // Regular method that calls another with function arg
  methodA(getArg) {
    console.log('methodA: this.name = ' + this.name);
    return this.methodB(getArg());
  }
  
  methodB(arg) {
    console.log('methodB: this.name = ' + this.name + ', arg = ' + arg);
    return this.name + ':' + arg;
  }
  
  // Method that returns a promise
  asyncMethod() {
    console.log('asyncMethod: this.name = ' + this.name);
    return Promise.resolve(this.name);
  }
}

function helperFunc() {
  return 'helper';
}

const obj1 = new StackTest('obj1');
const obj2 = new StackTest('obj2');

console.log('=== Test 1: Synchronous nested calls ===');
const result1 = obj1.methodA(() => helperFunc());
console.log('Result: ' + result1);

console.log('\n=== Test 2: Async then sync ===');
obj1.asyncMethod().then((val) => {
  console.log('In .then(), val = ' + val);
  // Now do a sync call with function arg
  const result = obj2.methodA(() => 'fromPromise');
  console.log('After sync call: ' + result);
});

console.log('\n=== Test 3: Multiple objects interleaved ===');
obj1.methodA(() => {
  obj2.methodB('nested');
  return 'test';
});

console.log('\n=== All tests done ===');
