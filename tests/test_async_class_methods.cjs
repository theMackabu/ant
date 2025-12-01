// Test async methods in classes

class AsyncTestClass {
  constructor() {
    this.name = 'AsyncTestClass';
    this.value = 42;
  }
  
  // Regular method
  regularMethod() {
    Ant.println('regularMethod: this.name = ' + this.name);
    return this.value;
  }
  
  // Async method
  async asyncMethod() {
    Ant.println('asyncMethod: this.name = ' + this.name);
    return Promise.resolve(this.value * 2);
  }
  
  // Async method with argument
  async asyncWithArg(multiplier) {
    Ant.println('asyncWithArg: this.name = ' + this.name + ', multiplier = ' + multiplier);
    return Promise.resolve(this.value * multiplier);
  }
  
  // Async method that calls helper
  async asyncWithHelper() {
    function helper() {
      return 10;
    }
    Ant.println('asyncWithHelper: this.name = ' + this.name);
    const extra = helper();
    return Promise.resolve(this.value + extra);
  }
}

const obj = new AsyncTestClass();

Ant.println('=== Test 1: Regular method ===');
const result1 = obj.regularMethod();
Ant.println('Result: ' + result1);

Ant.println('\n=== Test 2: Async method ===');
const promise2 = obj.asyncMethod();
Ant.println('Returned: ' + (typeof promise2));
promise2.then((val) => {
  Ant.println('Resolved value: ' + val);
  Ant.println('Inside .then(), this.name: ' + this.name);
});

Ant.println('\n=== Test 3: Async method with argument ===');
const promise3 = obj.asyncWithArg(5);
Ant.println('Returned: ' + (typeof promise3));
promise3.then((val) => {
  Ant.println('Resolved value: ' + val);
});

Ant.println('\n=== Test 4: Async method with helper function ===');
const promise4 = obj.asyncWithHelper();
Ant.println('Returned: ' + (typeof promise4));
promise4.then((val) => {
  Ant.println('Resolved value: ' + val);
});

Ant.println('\n=== All tests completed ===');
