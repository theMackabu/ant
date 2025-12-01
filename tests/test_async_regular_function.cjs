// Test that 'this' works with regular functions in async callbacks

class AsyncRegularFunctionTest {
  constructor(name) {
    this.name = name;
    this.value = 100;
  }
  
  testPromiseWithRegularFunction() {
    console.log('=== testPromiseWithRegularFunction ===');
    console.log('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then(function(val) {
      console.log('Inside .then() with function:');
      console.log('  typeof this: ' + typeof this);
      console.log('  this.name: ' + this.name);
      console.log('  this.value: ' + this.value);
      return val + 1;
    });
  }
  
  testPromiseWithArrowFunction() {
    console.log('\n=== testPromiseWithArrowFunction ===');
    console.log('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then((val) => {
      console.log('Inside .then() with arrow:');
      console.log('  typeof this: ' + typeof this);
      console.log('  this.name: ' + this.name);
      console.log('  this.value: ' + this.value);
      return val + 1;
    });
  }
  
  testMethodCallingPromiseWithFunction() {
    console.log('\n=== testMethodCallingPromiseWithFunction ===');
    const self = this;
    console.log('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(1).then(function() {
      console.log('In .then() with function:');
      console.log('  this.name: ' + this.name);
      console.log('  self.name: ' + self.name);
      return self.value;
    });
  }
}

const obj = new AsyncRegularFunctionTest('TestObject');

obj.testPromiseWithRegularFunction().then(function(result) {
  console.log('Result 1: ' + result);
});

obj.testPromiseWithArrowFunction().then((result) => {
  console.log('Result 2: ' + result);
});

obj.testMethodCallingPromiseWithFunction().then((result) => {
  console.log('Result 3: ' + result);
});

console.log('\n=== All tests queued ===');
