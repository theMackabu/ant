// Test async/promise handling with 'this' context

class AsyncClass {
  constructor() {
    this.name = 'AsyncClass';
    this.value = 100;
  }
  
  asyncMethod(arg) {
    Ant.println('asyncMethod called:');
    Ant.println('  this.name: ' + this.name);
    Ant.println('  this.value: ' + this.value);
    Ant.println('  arg: ' + arg);
    return Promise.resolve(this.value + 10);
  }
  
  methodWithPromise() {
    Ant.println('methodWithPromise called:');
    Ant.println('  this.name: ' + this.name);
    
    return Promise.resolve(this.value).then((v) => {
      Ant.println('  Inside .then(), this.name: ' + this.name);
      Ant.println('  Inside .then(), this.value: ' + this.value);
      return v * 2;
    });
  }
  
  nestedAsync(helperFunc) {
    Ant.println('nestedAsync called:');
    Ant.println('  this.name: ' + this.name);
    const result = helperFunc();
    Ant.println('  After helper call, this.name: ' + this.name);
    return result;
  }
}

function helperFunction() {
  return 'helper result';
}

const obj = new AsyncClass();

Ant.println('=== Test 1: Async method ===');
const p1 = obj.asyncMethod('test');
Ant.println('Promise returned: ' + (typeof p1));

Ant.println('\n=== Test 2: Method with promise chain ===');
const p2 = obj.methodWithPromise();
Ant.println('Promise returned: ' + (typeof p2));

Ant.println('\n=== Test 3: Nested call with helper ===');
obj.nestedAsync(helperFunction);

Ant.println('\n=== All tests completed ===');
