// Demonstration of 'this' keyword and 'new' operator

console.log("=== Constructor Functions with 'new' and 'this' ===\n");

// Basic constructor function
function Person(name, age) {
  this.name = name;
  this.age = age;
  this.greet = function() {
    return "Hello, my name is " + this.name + " and I am " + this.age + " years old.";
  };
}

// Create instances using 'new'
console.log("Creating person1 with new Person('Alice', 30)");
let person1 = new Person("Alice", 30);
console.log("person1.name:", person1.name);
console.log("person1.age:", person1.age);
console.log("person1.greet():", person1.greet());
console.log();

console.log("Creating person2 with new Person('Bob', 25)");
let person2 = new Person("Bob", 25);
console.log("person2.name:", person2.name);
console.log("person2.age:", person2.age);
console.log("person2.greet():", person2.greet());
console.log();

// Each instance has its own 'this' context
console.log("=== Independent 'this' contexts ===");
console.log("person1.name:", person1.name);
console.log("person2.name:", person2.name);
console.log("They are different instances with independent 'this' values");
console.log();

// Constructor with methods that use 'this'
function Counter(start) {
  this.value = start;
  this.increment = function() {
    this.value = this.value + 1;
    return this.value;
  };
  this.decrement = function() {
    this.value = this.value - 1;
    return this.value;
  };
  this.getValue = function() {
    return this.value;
  };
}

console.log("=== Counter Example ===");
let counter = new Counter(10);
console.log("Initial value:", counter.getValue());
console.log("After increment:", counter.increment());
console.log("After increment:", counter.increment());
console.log("After decrement:", counter.decrement());
console.log("Final value:", counter.getValue());
console.log();

// Multiple counters with independent state
console.log("=== Multiple Independent Counters ===");
let counter1 = new Counter(0);
let counter2 = new Counter(100);

console.log("counter1 initial:", counter1.getValue());
console.log("counter2 initial:", counter2.getValue());

counter1.increment();
counter1.increment();
counter2.decrement();

console.log("counter1 after 2 increments:", counter1.getValue());
console.log("counter2 after 1 decrement:", counter2.getValue());
console.log();

// Constructor with computed properties
function Rectangle(width, height) {
  this.width = width;
  this.height = height;
  this.area = function() {
    return this.width * this.height;
  };
  this.perimeter = function() {
    return 2 * (this.width + this.height);
  };
}

console.log("=== Rectangle Example ===");
let rect1 = new Rectangle(5, 10);
console.log("Rectangle 5x10:");
console.log("  Width:", rect1.width);
console.log("  Height:", rect1.height);
console.log("  Area:", rect1.area());
console.log("  Perimeter:", rect1.perimeter());
console.log();

let rect2 = new Rectangle(3, 7);
console.log("Rectangle 3x7:");
console.log("  Width:", rect2.width);
console.log("  Height:", rect2.height);
console.log("  Area:", rect2.area());
console.log("  Perimeter:", rect2.perimeter());
console.log();

// 'this' refers to the object being constructed
function Car(make, model, year) {
  this.make = make;
  this.model = model;
  this.year = year;
  this.displayInfo = function() {
    return this.year + " " + this.make + " " + this.model;
  };
}

console.log("=== Car Example ===");
let car1 = new Car("Toyota", "Camry", 2020);
let car2 = new Car("Honda", "Civic", 2021);

console.log("car1:", car1.displayInfo());
console.log("car2:", car2.displayInfo());
console.log();

// 'this' in nested functions
function Account(owner, balance) {
  this.owner = owner;
  this.balance = balance;
  
  this.deposit = function(amount) {
    this.balance = this.balance + amount;
    return this.balance;
  };
  
  this.withdraw = function(amount) {
    this.balance = this.balance - amount;
    return this.balance;
  };
  
  this.getInfo = function() {
    return this.owner + " has $" + this.balance;
  };
}

console.log("=== Bank Account Example ===");
let account1 = new Account("Alice", 1000);
console.log("Initial:", account1.getInfo());

account1.deposit(500);
console.log("After deposit 500:", account1.getInfo());

account1.withdraw(200);
console.log("After withdraw 200:", account1.getInfo());
console.log();

// Multiple accounts
let account2 = new Account("Bob", 2000);
console.log("account1:", account1.getInfo());
console.log("account2:", account2.getInfo());
console.log();

console.log("=== Summary ===");
console.log("- 'new' creates a new object");
console.log("- 'this' inside constructor refers to the new object");
console.log("- Each instance has its own 'this' context");
console.log("- Methods can access and modify 'this' properties");
