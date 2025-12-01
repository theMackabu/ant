// Test class keyword

console.log("=== Testing class keyword ===");

// Test 1: Simple class with constructor
class Person {
  constructor(name, age) {
    this.name = name;
    this.age = age;
  }
}

let person1 = new Person("Alice", 30);
console.log(person1.name);  // "Alice"
console.log(person1.age);   // 30

// Test 2: Class with methods
class Calculator {
  constructor(value) {
    this.value = value;
  }
  
  add(n) {
    this.value = this.value + n;
    return this.value;
  }
  
  subtract(n) {
    this.value = this.value - n;
    return this.value;
  }
  
  getValue() {
    return this.value;
  }
}

let calc = new Calculator(10);
console.log(calc.value);      // 10
console.log(calc.add(5));     // 15
console.log(calc.subtract(3)); // 12
console.log(calc.getValue()); // 12

// Test 3: Class without explicit constructor
class Point {
  setCoords(x, y) {
    this.x = x;
    this.y = y;
  }
  
  getDistance() {
    return this.x * this.x + this.y * this.y;
  }
}

let p = new Point();
p.setCoords(3, 4);
console.log(p.x);  // 3
console.log(p.y);  // 4
console.log(p.getDistance());  // 25

// Test 4: Multiple instances
class Counter {
  constructor(start) {
    this.count = start;
  }
  
  increment() {
    this.count = this.count + 1;
    return this.count;
  }
}

let counter1 = new Counter(0);
let counter2 = new Counter(10);
console.log(counter1.increment());  // 1
console.log(counter1.increment());  // 2
console.log(counter2.increment());  // 11
console.log(counter2.increment());  // 12

// Test 5: Class instances are objects
let person2 = new Person("Bob", 25);
console.log(typeof person2);  // object
console.log(person2 instanceof Object);  // true

// Test 6: Modify instance properties
class Rectangle {
  constructor(width, height) {
    this.width = width;
    this.height = height;
  }
  
  area() {
    return this.width * this.height;
  }
}

let rect = new Rectangle(5, 10);
console.log(rect.area());  // 50
rect.width = 8;
console.log(rect.area());  // 80

// Test 7: Delete class instance properties
let person3 = new Person("Charlie", 35);
console.log(person3.name);  // "Charlie"
delete person3.name;
console.log(person3.name);  // undefined
console.log(person3.age);   // 35

// Test 8: Class with property management  
class Store {
  constructor() {
    this.items = 0;
  }
  
  addItem(name, value) {
    this.items = this.items + 1;
  }
  
  removeItem(name) {
    this.items = this.items - 1;
    return true;
  }
  
  getItemCount() {
    return this.items;
  }
}

let store = new Store();
console.log(store.getItemCount());  // 0
store.addItem("apple", 5);
store.addItem("banana", 3);
console.log(store.getItemCount());  // 2
store.removeItem("apple");
console.log(store.getItemCount());  // 1

// Test 9: Method chaining
class ChainableCounter {
  constructor(value) {
    this.value = value;
  }
  
  add(n) {
    this.value = this.value + n;
    return this;
  }
  
  multiply(n) {
    this.value = this.value * n;
    return this;
  }
  
  get() {
    return this.value;
  }
}

let chain = new ChainableCounter(5);
let result = chain.add(3).multiply(2).get();
console.log(result);  // 16

// Test 10: Class with boolean properties
class Feature {
  constructor(name) {
    this.name = name;
    this.enabled = false;
  }
  
  enable() {
    this.enabled = true;
  }
  
  disable() {
    this.enabled = false;
  }
  
  isEnabled() {
    return this.enabled;
  }
}

let feature = new Feature("DarkMode");
console.log(feature.isEnabled());  // false
feature.enable();
console.log(feature.isEnabled());  // true
feature.disable();
console.log(feature.isEnabled());  // false

console.log("=== Class tests completed ===");
