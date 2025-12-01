// Test explicit this binding in async contexts

class TestClass {
  constructor(name) {
    this.name = name;
  }
  
  methodThatReturnsPromise() {
    console.log('methodThatReturnsPromise: this.name = ' + this.name);
    
    // Use regular function (not arrow) in .then()
    return Promise.resolve('value').then(function(val) {
      // In standard JS, 'this' would be undefined here
      // Let's see what Ant does
      console.log('In .then() regular function:');
      console.log('  typeof this: ' + typeof this);
      console.log('  this: ' + this);
      
      // Try to access properties
      if (typeof this.name !== 'undefined') {
        console.log('  this.name: ' + this.name);
      } else {
        console.log('  this.name is undefined');
      }
      
      return val;
    });
  }
}

const obj1 = new TestClass('Object1');
const obj2 = new TestClass('Object2');

console.log('=== Calling obj1.methodThatReturnsPromise() ===');
obj1.methodThatReturnsPromise().then(function(result) {
  console.log('Final result: ' + result);
});

console.log('\n=== Calling obj2.methodThatReturnsPromise() ===');
obj2.methodThatReturnsPromise().then(function(result) {
  console.log('Final result: ' + result);
});

console.log('\n=== Tests queued ===');
