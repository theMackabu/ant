// Test class function (method) hoisting behavior
// Class methods can be called regardless of their definition order within the class body
// But classes themselves are NOT hoisted like function declarations

console.log("=== Test 1: Methods calling other methods defined later ===");
class Calculator {
  calculate(x) {
    return this.double(x) + this.triple(x);
  }
  
  double(x) {
    return x * 2;
  }
  
  triple(x) {
    return x * 3;
  }
}

let calc = new Calculator();
console.log("calculate(5) should be 25:", calc.calculate(5));

console.log("\n=== Test 2: Constructor calling methods defined after it ===");
class Greeter {
  constructor(name) {
    this.name = name;
    this.greeting = this.makeGreeting();
  }
  
  makeGreeting() {
    return "Hello, " + this.name + "!";
  }
}

let greeter = new Greeter("World");
console.log("greeting should be 'Hello, World!':", greeter.greeting);

console.log("\n=== Test 3: Method calling method defined before constructor ===");
class Parser {
  parse(input) {
    return this.validate(input);
  }
  
  constructor() {
    this.initialized = true;
  }
  
  validate(input) {
    return input.length > 0;
  }
}

let parser = new Parser();
console.log("parse('test') should be true:", parser.parse("test"));

console.log("\n=== Test 4: Mutual method references ===");
class Counter {
  increment() {
    this.value = this.getValue() + 1;
  }
  
  decrement() {
    this.value = this.getValue() - 1;
  }
  
  constructor(initial) {
    this.value = initial;
  }
  
  getValue() {
    return this.value;
  }
}

let counter = new Counter(10);
counter.increment();
console.log("after increment should be 11:", counter.getValue());
counter.decrement();
counter.decrement();
console.log("after two decrements should be 9:", counter.getValue());

console.log("\n=== Test 5: Deep method call chain ===");
class Chain {
  a() {
    return this.b() + 1;
  }
  
  b() {
    return this.c() + 2;
  }
  
  c() {
    return this.d() + 3;
  }
  
  d() {
    return 10;
  }
}

let chain = new Chain();
console.log("a() should be 16 (10+3+2+1):", chain.a());

console.log("\n=== Test 6: Class NOT hoisted (should error if accessed before) ===");
try {
  let early = new NotYetDefined();
  console.log("ERROR: Should not reach here");
} catch (e) {
  console.log("Correctly caught error - class not hoisted");
}

class NotYetDefined {
  constructor() {
    this.value = 42;
  }
}

let late = new NotYetDefined();
console.log("After definition, value should be 42:", late.value);

console.log("\n=== Test 7: Static methods hoisting within class ===");
class StaticTest {
  static compute() {
    return StaticTest.helper() * 2;
  }
  
  static helper() {
    return 21;
  }
}

console.log("StaticTest.compute() should be 42:", StaticTest.compute());

console.log("\n=== Test 8: Getter/setter order independence ===");
class GetSet {
  get doubled() {
    return this._val * 2;
  }
  
  constructor(val) {
    this._val = val;
  }
  
  set doubled(val) {
    this._val = val / 2;
  }
}

let gs = new GetSet(5);
console.log("doubled should be 10:", gs.doubled);
gs.doubled = 20;
console.log("after setting doubled=20, _val should be 10:", gs._val);

console.log("\n=== All class function hoisting tests completed ===");
