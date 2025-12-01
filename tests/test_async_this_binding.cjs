// Test explicit this binding in async contexts

class TestClass {
  constructor(name) {
    this.name = name;
  }
  
  methodThatReturnsPromise() {
    Ant.println('methodThatReturnsPromise: this.name = ' + this.name);
    
    // Use regular function (not arrow) in .then()
    return Promise.resolve('value').then(function(val) {
      // In standard JS, 'this' would be undefined here
      // Let's see what Ant does
      Ant.println('In .then() regular function:');
      Ant.println('  typeof this: ' + typeof this);
      Ant.println('  this: ' + this);
      
      // Try to access properties
      if (typeof this.name !== 'undefined') {
        Ant.println('  this.name: ' + this.name);
      } else {
        Ant.println('  this.name is undefined');
      }
      
      return val;
    });
  }
}

const obj1 = new TestClass('Object1');
const obj2 = new TestClass('Object2');

Ant.println('=== Calling obj1.methodThatReturnsPromise() ===');
obj1.methodThatReturnsPromise().then(function(result) {
  Ant.println('Final result: ' + result);
});

Ant.println('\n=== Calling obj2.methodThatReturnsPromise() ===');
obj2.methodThatReturnsPromise().then(function(result) {
  Ant.println('Final result: ' + result);
});

Ant.println('\n=== Tests queued ===');
