// Test that 'this' works with regular functions in async callbacks

class AsyncRegularFunctionTest {
  constructor(name) {
    this.name = name;
    this.value = 100;
  }
  
  testPromiseWithRegularFunction() {
    Ant.println('=== testPromiseWithRegularFunction ===');
    Ant.println('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then(function(val) {
      Ant.println('Inside .then() with function:');
      Ant.println('  typeof this: ' + typeof this);
      Ant.println('  this.name: ' + this.name);
      Ant.println('  this.value: ' + this.value);
      return val + 1;
    });
  }
  
  testPromiseWithArrowFunction() {
    Ant.println('\n=== testPromiseWithArrowFunction ===');
    Ant.println('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then((val) => {
      Ant.println('Inside .then() with arrow:');
      Ant.println('  typeof this: ' + typeof this);
      Ant.println('  this.name: ' + this.name);
      Ant.println('  this.value: ' + this.value);
      return val + 1;
    });
  }
  
  testMethodCallingPromiseWithFunction() {
    Ant.println('\n=== testMethodCallingPromiseWithFunction ===');
    const self = this;
    Ant.println('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(1).then(function() {
      Ant.println('In .then() with function:');
      Ant.println('  this.name: ' + this.name);
      Ant.println('  self.name: ' + self.name);
      return self.value;
    });
  }
}

const obj = new AsyncRegularFunctionTest('TestObject');

obj.testPromiseWithRegularFunction().then(function(result) {
  Ant.println('Result 1: ' + result);
});

obj.testPromiseWithArrowFunction().then((result) => {
  Ant.println('Result 2: ' + result);
});

obj.testMethodCallingPromiseWithFunction().then((result) => {
  Ant.println('Result 3: ' + result);
});

Ant.println('\n=== All tests queued ===');
