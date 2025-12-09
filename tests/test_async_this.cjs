// Test async/promise handling with 'this' context

class AsyncClass {
  constructor() {
    this.name = 'AsyncClass';
    this.value = 100;
  }
  
  asyncMethod(arg) {
    console.log('asyncMethod called:');
    console.log('  this.name: ' + this.name);
    console.log('  this.value: ' + this.value);
    console.log('  arg: ' + arg);
    return Promise.resolve(this.value + 10);
  }
  
  methodWithPromise() {
    console.log('methodWithPromise called:');
    console.log('  this.name: ' + this.name);
    const self = this;
    
    return Promise.resolve(this.value).then(function(v) {
      console.log('  Inside .then(), this.name: ' + self.name);
      console.log('  Inside .then(), this.value: ' + self.value);
      return v * 2;
    });
  }
  
  nestedAsync(helperFunc) {
    console.log('nestedAsync called:');
    console.log('  this.name: ' + this.name);
    const result = helperFunc();
    console.log('  After helper call, this.name: ' + this.name);
    return result;
  }
}

function helperFunction() {
  return 'helper result';
}

const obj = new AsyncClass();

console.log('=== Test 1: Async method ===');
const p1 = obj.asyncMethod('test');
console.log('Promise returned: ' + (typeof p1));

console.log('\n=== Test 2: Method with promise chain ===');
const p2 = obj.methodWithPromise();
console.log('Promise returned: ' + (typeof p2));

console.log('\n=== Test 3: Nested call with helper ===');
obj.nestedAsync(helperFunction);

console.log('\n=== All tests completed ===');
