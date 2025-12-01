// Test that 'this' is preserved in async callbacks

class AsyncContextTest {
  constructor(name) {
    this.name = name;
    this.value = 100;
  }
  
  testPromiseThen() {
    Ant.println('=== testPromiseThen ===');
    Ant.println('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(42).then((val) => {
      Ant.println('Inside .then(): this.name = ' + this.name);
      Ant.println('Inside .then(): this.value = ' + this.value);
      return this.value + val;
    });
  }
  
  testNestedPromises() {
    Ant.println('\n=== testNestedPromises ===');
    Ant.println('Before promise: this.name = ' + this.name);
    
    return Promise.resolve(10).then((val1) => {
      Ant.println('First .then(): this.name = ' + this.name);
      return Promise.resolve(val1 + this.value).then((val2) => {
        Ant.println('Second .then(): this.name = ' + this.name);
        return val2 * 2;
      });
    });
  }
  
  testMethodCallingAsync() {
    Ant.println('\n=== testMethodCallingAsync ===');
    Ant.println('Before: this.name = ' + this.name);
    
    const helper = () => {
      Ant.println('Inside helper: this.name = ' + this.name);
      return this.value;
    };
    
    return Promise.resolve(1).then(() => {
      Ant.println('In .then(), calling helper');
      return helper();
    });
  }
}

const obj = new AsyncContextTest('TestObject');

obj.testPromiseThen().then((result) => {
  Ant.println('Result 1: ' + result);
});

obj.testNestedPromises().then((result) => {
  Ant.println('Result 2: ' + result);
});

obj.testMethodCallingAsync().then((result) => {
  Ant.println('Result 3: ' + result);
});

Ant.println('\n=== All tests queued ===');
