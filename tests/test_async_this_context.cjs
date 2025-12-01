// Test that 'this' is preserved in async callbacks

class AsyncContextTest {
  constructor(name) {
    this.name = name;
    this.value = 100;
  }
  
  testPromiseThen() {
    console.log('=== testPromiseThen ===');
    console.log('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then((val) => {
      console.log('Inside .then(): this.name = ' + this.name);
      console.log('Inside .then(): this.value = ' + this.value);
      return this.value + val;
    });
  }
  
  testNestedPromises() {
    console.log('\n=== testNestedPromises ===');
    console.log('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(10).then((val1) => {
      console.log('First .then(): this.name = ' + this.name);
      return Promise.resolve(val1 + this.value).then((val2) => {
        console.log('Second .then(): this.name = ' + this.name);
        return val2 * 2;
      });
    });
  }
  
  testMethodCallingAsync() {
    console.log('\n=== testMethodCallingAsync ===');
    console.log('Before: this.name = ' + this.name);
    
    const helper = () => {
      console.log('Inside helper: this.name = ' + this.name);
      return this.value;
    };
    
    return Promise.resolve(1).then(() => {
      console.log('In .then(), calling helper');
      return helper();
    });
  }
}

const obj = new AsyncContextTest('TestObject');

obj.testPromiseThen().then((result) => {
  console.log('Result 1: ' + result);
});

obj.testNestedPromises().then((result) => {
  console.log('Result 2: ' + result);
});

obj.testMethodCallingAsync().then((result) => {
  console.log('Result 3: ' + result);
});

console.log('\n=== All tests queued ===');
