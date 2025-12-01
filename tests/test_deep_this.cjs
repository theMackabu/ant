// Test deeply nested calls with 'this' context

class DeepClass {
  constructor(name) {
    this.name = name;
    this.depth = 0;
  }
  
  method1(func) {
    console.log('method1: this.name = ' + this.name + ', depth = ' + this.depth);
    this.depth++;
    const result = func();
    this.depth--;
    return result;
  }
  
  method2(func) {
    console.log('method2: this.name = ' + this.name + ', depth = ' + this.depth);
    this.depth++;
    const result = func();
    this.depth--;
    return result;
  }
  
  method3(func) {
    console.log('method3: this.name = ' + this.name + ', depth = ' + this.depth);
    this.depth++;
    const result = func();
    this.depth--;
    return result;
  }
  
  leaf() {
    console.log('leaf: this.name = ' + this.name + ', depth = ' + this.depth);
    return 'done';
  }
}

function helper1() {
  return 'helper1';
}

function helper2() {
  return 'helper2';
}

const obj1 = new DeepClass('obj1');
const obj2 = new DeepClass('obj2');

console.log('=== Test 1: Deeply nested calls on same object ===');
obj1.method1(() => {
  return obj1.method2(() => {
    return obj1.method3(() => {
      return obj1.leaf();
    });
  });
});

console.log('\n=== Test 2: Interleaved calls on different objects ===');
obj1.method1(() => {
  obj2.method1(() => {
    return obj2.leaf();
  });
  return obj1.method2(() => {
    return obj1.leaf();
  });
});

console.log('\n=== Test 3: With helper function calls in arguments ===');
obj1.method1(() => helper1());
obj2.method2(() => helper2());

console.log('\n=== All deep nesting tests passed ===');
