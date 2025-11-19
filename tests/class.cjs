// Test class keyword

Ant.println("=== Testing class keyword ===");

// Test 1: Simple class with constructor
class Person {
  constructor(name, age) {
    this.name = name;
    this.age = age;
  }
}

let person1 = new Person("Alice", 30);
Ant.println(person1.name);  // "Alice"
Ant.println(person1.age);   // 30

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
Ant.println(calc.value);      // 10
Ant.println(calc.add(5));     // 15
Ant.println(calc.subtract(3)); // 12
Ant.println(calc.getValue()); // 12

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
Ant.println(p.x);  // 3
Ant.println(p.y);  // 4
Ant.println(p.getDistance());  // 25

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
Ant.println(counter1.increment());  // 1
Ant.println(counter1.increment());  // 2
Ant.println(counter2.increment());  // 11
Ant.println(counter2.increment());  // 12

// Test 5: Class instances are objects
let person2 = new Person("Bob", 25);
Ant.println(typeof person2);  // object
Ant.println(person2 instanceof Object);  // true

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
Ant.println(rect.area());  // 50
rect.width = 8;
Ant.println(rect.area());  // 80

// Test 7: Delete class instance properties
let person3 = new Person("Charlie", 35);
Ant.println(person3.name);  // "Charlie"
delete person3.name;
Ant.println(person3.name);  // undefined
Ant.println(person3.age);   // 35

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
Ant.println(store.getItemCount());  // 0
store.addItem("apple", 5);
store.addItem("banana", 3);
Ant.println(store.getItemCount());  // 2
store.removeItem("apple");
Ant.println(store.getItemCount());  // 1

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
Ant.println(result);  // 16

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
Ant.println(feature.isEnabled());  // false
feature.enable();
Ant.println(feature.isEnabled());  // true
feature.disable();
Ant.println(feature.isEnabled());  // false

Ant.println("=== Class tests completed ===");
