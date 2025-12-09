// Test async methods in classes

class AsyncTestClass {
  constructor() {
    this.name = 'AsyncTestClass';
    this.value = 42;
  }

  // Regular method
  regularMethod() {
    console.log('regularMethod: this.name = ' + this.name);
    return this.value;
  }

  // Async method
  async asyncMethod() {
    console.log('asyncMethod: this.name = ' + this.name);
    return Promise.resolve(this.value * 2);
  }

  // Async method with argument
  async asyncWithArg(multiplier) {
    console.log('asyncWithArg: this.name = ' + this.name + ', multiplier = ' + multiplier);
    return Promise.resolve(this.value * multiplier);
  }

  // Async method that calls helper
  async asyncWithHelper() {
    function helper() {
      return 10;
    }
    console.log('asyncWithHelper: this.name = ' + this.name);
    const extra = helper();
    return Promise.resolve(this.value + extra);
  }
}

const obj = new AsyncTestClass();

console.log('=== Test 1: Regular method ===');
const result1 = obj.regularMethod();
console.log('Result: ' + result1);

console.log('\n=== Test 2: Async method ===');
const promise2 = obj.asyncMethod();
console.log('Returned: ' + typeof promise2);
promise2.then(function (val) {
  console.log('Resolved value: ' + val);
});

console.log('\n=== Test 3: Async method with argument ===');
const promise3 = obj.asyncWithArg(5);
console.log('Returned: ' + typeof promise3);
promise3.then(function (val) {
  console.log('Resolved value: ' + val);
});

console.log('\n=== Test 4: Async method with helper function ===');
const promise4 = obj.asyncWithHelper();
console.log('Returned: ' + typeof promise4);
promise4.then(function (val) {
  console.log('Resolved value: ' + val);
});

console.log('\n=== All tests completed ===');
