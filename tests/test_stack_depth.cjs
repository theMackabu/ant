// Test that the this stack is properly managed across async boundaries

class StackTest {
  constructor(name) {
    this.name = name;
  }
  
  // Regular method that calls another with function arg
  methodA(getArg) {
    Ant.println('methodA: this.name = ' + this.name);
    return this.methodB(getArg());
  }
  
  methodB(arg) {
    Ant.println('methodB: this.name = ' + this.name + ', arg = ' + arg);
    return this.name + ':' + arg;
  }
  
  // Method that returns a promise
  asyncMethod() {
    Ant.println('asyncMethod: this.name = ' + this.name);
    return Promise.resolve(this.name);
  }
}

function helperFunc() {
  return 'helper';
}

const obj1 = new StackTest('obj1');
const obj2 = new StackTest('obj2');

Ant.println('=== Test 1: Synchronous nested calls ===');
const result1 = obj1.methodA(() => helperFunc());
Ant.println('Result: ' + result1);

Ant.println('\n=== Test 2: Async then sync ===');
obj1.asyncMethod().then((val) => {
  Ant.println('In .then(), val = ' + val);
  // Now do a sync call with function arg
  const result = obj2.methodA(() => 'fromPromise');
  Ant.println('After sync call: ' + result);
});

Ant.println('\n=== Test 3: Multiple objects interleaved ===');
obj1.methodA(() => {
  obj2.methodB('nested');
  return 'test';
});

Ant.println('\n=== All tests done ===');
